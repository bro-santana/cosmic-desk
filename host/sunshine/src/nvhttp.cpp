/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <chrono>  // COSMIC MODIFICATION: TTL bookkeeping for the endpoint->cert-fingerprint cache.
#include <cstdlib>  // COSMIC MODIFICATION: std::strtoull for GET /cosmic/clipboard's "since" query arg.
#include <filesystem>
#include <format>
#include <map>  // COSMIC MODIFICATION: endpoint->cert-fingerprint cache backing store.
#include <mutex>  // COSMIC MODIFICATION: guards the endpoint->cert-fingerprint cache.
#include <string>
#include <string_view>  // COSMIC MODIFICATION: raw-digest view in cosmic_cert_fingerprint().
#include <utility>

// lib includes
#include <boost/algorithm/string.hpp>  // COSMIC MODIFICATION: case-insensitive Accept/Content-Type parsing for /cosmic/clipboard.
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
// COSMIC MODIFICATION: system_tray.h deleted — Cosmic Desk has its own tray (M0).
#include "utility.h"
#include "uuid.h"
#include "video.h"
// COSMIC MODIFICATION: hook into Cosmic Desk's native PIN dialog (M1.4). The
// header lives in the app's src/ tree; only the declaration is needed here —
// pin_bridge.cpp is compiled into the cosmicdesk target, not cosmic_host.
#include "hostglue/pin_bridge.h"
// COSMIC MODIFICATION: Cosmic Desk display enumeration for the /serverinfo
// extension (M5.1). Same pattern as pin_bridge.h: declared in the app's src/
// tree, implemented in displays.cpp in the cosmicdesk target.
#include "hostglue/displays.h"
// COSMIC MODIFICATION: wallpaper hash/bytes provider for the /serverinfo
// extension and the /cosmic/wallpaper route (D10a/b). Same pattern as
// displays.h: declared in the app's src/ tree, implemented in wallpaper.cpp
// in the cosmicdesk target.
#include "hostglue/wallpaper.h"
// COSMIC MODIFICATION: clipboard text bridge for the GET/POST
// /cosmic/clipboard routes. Same pattern as wallpaper.h: declared in the
// app's src/ tree, implemented in clipboard.cpp in the cosmicdesk target.
#include "hostglue/clipboard.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;

  // COSMIC MODIFICATION: per-connection client-certificate identity, used to
  // gate GET/POST /cosmic/clipboard to the single paired client that started
  // (via /launch or /resume) the currently active stream session -- see the
  // comment above cosmic_clipboard_get further down. Defined here, right
  // after cert_chain, because launch() (the earliest caller) needs it and
  // C++ requires these to be defined before first use.

  // SHA-256 fingerprint of a verified peer certificate, hex-encoded
  // uppercase. The exact encoding is an implementation detail: the value is
  // only ever compared against another value produced by this same
  // function, never against an externally supplied string.
  std::string cosmic_cert_fingerprint(X509 *x509) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (X509_digest(x509, EVP_sha256(), digest, &digest_len) != 1 || digest_len == 0) {
      return {};
    }

    // util::hex_vec's default byte order is reversed, and its `--end` on an
    // empty range is UB (see utility.h:300-341), so rev=true is required
    // here -- not a style choice.
    return util::hex_vec(std::string_view(reinterpret_cast<const char *>(digest), digest_len), true);
  }

  // COSMIC MODIFICATION: caches the fingerprint the verify hook
  // (https_server.verify below) associated with a live TCP connection,
  // keyed by that connection's remote endpoint. Entries are inserted only
  // for successfully verified TLS connections, and a live connection
  // re-touches its entry on every clipboard/launch/resume request
  // (cosmic_request_cert_fingerprint refreshes last_used on every hit), so
  // pruning on insert with a generous TTL bounds the map's size. A
  // connection that only ever calls /serverinfo, /applist, /appasset or
  // /cosmic/wallpaper never refreshes its entry and can be pruned while
  // still open; that connection's later clipboard/launch/resume requests
  // then simply miss the cache (cosmic_request_cert_fingerprint returns
  // {}), which fails closed -- a clipboard request 404s and a launch/resume
  // records an empty owner -- rather than misattributing the connection to
  // someone else. The remote endpoint (address + port) uniquely identifies
  // a live connection to this server for a given local endpoint; two
  // simultaneous connections can collide on this key only on a multi-homed
  // host (net::get_bind_address defaults to the wildcard address, see
  // network.cpp:120-128) where a client deliberately reuses its source port
  // against two different local addresses.
  struct cosmic_cert_cache_entry_t {
    std::string fingerprint;
    std::chrono::steady_clock::time_point last_used;
  };

  std::mutex cosmic_cert_cache_mutex;
  std::map<boost::asio::ip::tcp::endpoint, cosmic_cert_cache_entry_t> cosmic_cert_cache;

  void cosmic_remember_client_cert(const boost::asio::ip::tcp::endpoint &endpoint, std::string fingerprint) {
    // A default-constructed endpoint (port 0) is what Request::remote_endpoint()
    // yields on error -- the key is genuinely unknown there, so skip it
    // without touching the map.
    if (endpoint.port() == 0) {
      return;
    }

    constexpr auto kCosmicCertCacheTtl = std::chrono::minutes(10);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(cosmic_cert_cache_mutex);
    std::erase_if(cosmic_cert_cache, [now, kCosmicCertCacheTtl](const auto &entry) {
      return now - entry.second.last_used > kCosmicCertCacheTtl;
    });

    // COSMIC MODIFICATION: an empty fingerprint means verification produced
    // no usable identity for this connection. Erase any stale entry for the
    // endpoint instead of just returning -- otherwise a prior occupant's
    // fingerprint could survive and later be misattributed to a different
    // client that reuses the same source port.
    if (fingerprint.empty()) {
      cosmic_cert_cache.erase(endpoint);
      return;
    }

    cosmic_cert_cache[endpoint] = cosmic_cert_cache_entry_t {std::move(fingerprint), now};
  }

  std::string cosmic_request_cert_fingerprint(const boost::asio::ip::tcp::endpoint &endpoint) {
    if (endpoint.port() == 0) {
      return {};
    }

    std::lock_guard lock(cosmic_cert_cache_mutex);
    auto it = cosmic_cert_cache.find(endpoint);
    if (it == cosmic_cert_cache.end()) {
      return {};
    }

    it->second.last_used = std::chrono::steady_clock::now();
    return it->second.fingerprint;
  }

  // COSMIC MODIFICATION: true when header's Accept value contains the
  // case-insensitive literal "image/png". CosmicVersion 5 clients advertise
  // PNG clipboard support this way; v1-v4 clients send "Accept: */*" or omit
  // the header entirely, so this returns false for them -- see
  // cosmic_clipboard_get, which uses that to keep an image entry from being
  // served as text to an old client.
  bool cosmic_request_accepts_png(const SimpleWeb::CaseInsensitiveMultimap &header) {
    auto it = header.find("Accept");
    if (it == header.end()) {
      return false;
    }
    return boost::algorithm::icontains(it->second, "image/png"sv);
  }

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    // COSMIC MODIFICATION: the hook now also receives the connection's
    // remote endpoint, so the verified client certificate can be associated
    // (via cosmic_remember_client_cert() above) with the connection that
    // request handlers later identify via Request::remote_endpoint().
    std::function<int(SSL *, const boost::asio::ip::tcp::endpoint &)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              // COSMIC MODIFICATION: pass the connection's remote endpoint
              // through so the verify hook can record which client
              // certificate authenticated this connection (see the comment
              // on `verify` above).
              if (verify && !verify(session->connection->socket->native_handle(), session->request->remote_endpoint())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
    bool enabled = true;
  };

  struct client_t {
    std::vector<named_cert_t> named_devices;
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  void save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      pt::write_json(config::nvhttp.file_state, root);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        client.named_devices.emplace_back(named_cert);
      }
    }

    // Empty certificate chain and import certs from file
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(crypto::x509(named_cert.cert));
    }

    client_root = client;
  }

  void add_authorized_client(const std::string &name, std::string &&cert) {
    client_t &client = client_root;
    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(cert);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    client.named_devices.emplace_back(named_cert);

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    map_id_sess.erase(sess.client.uniqueID);
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      // The client is now successfully paired and will be authorized to connect
      add_authorized_client(client.name, std::move(client.cert));
    } else {
      tree.put("root.paired", 0);
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template<class T>
  void pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(debug) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
          return;
        } else {
          // COSMIC MODIFICATION: system_tray::update_tray_require_pin() removed —
          // Cosmic Desk surfaces pairing via its own PIN dialog (M1.4).
          ptr->second.async_insert_pin.response = std::move(response);

          // COSMIC MODIFICATION: surface the pending pairing request to the app's
          // main thread, which raises the window and shows the PIN dialog (M1.4).
          cosmic::pin_bridge::notify_pair_request(ptr->second.client.uniqueID);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool pin(std::string pin, std::string name) {
    pt::ptree tree;
    if (map_id_sess.empty()) {
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        std::format("Pin must be 4 digits, {} provided", pin.size())
      );
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = std::begin(map_id_sess)->second;
    getservercert(sess, tree, pin);
    sess.client.name = name;

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    } else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    } else {
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // COSMIC MODIFICATION: Cosmic Desk display extension (PLAN D3a, docs/PROTOCOL.md).
    // Stock Moonlight clients ignore unknown nodes. Ordering contract (D3c): these
    // entries are in platf::display_names() order, the same order consumed by
    // apply_shortcut()'s Ctrl+Alt+Shift+F(1+i) handler in input.cpp. Version 2
    // adds CosmicWallpaperHash (PLAN D10a/b, milestone W1 item 2). Version 3
    // adds the GET/POST /cosmic/clipboard routes. Version 4 adds wait=1
    // long-polling to GET /cosmic/clipboard. Version 5 adds image/png
    // clipboard payloads to both /cosmic/clipboard routes.
    tree.put("root.CosmicVersion", 5);
    pt::ptree displays_tree;
    int index = 0;
    for (const auto &display : cosmic::displays::list_displays()) {
      pt::ptree node;
      node.put("<xmlattr>.index", index);
      node.put("<xmlattr>.name", display.name);
      node.put("<xmlattr>.width", display.width);
      node.put("<xmlattr>.height", display.height);
      node.put("<xmlattr>.fps", display.fps);
      node.put("<xmlattr>.primary", display.primary ? 1 : 0);
      node.put("<xmlattr>.active", display.active ? 1 : 0);
      displays_tree.add_child("Display", node);
      ++index;
    }
    tree.add_child("root.CosmicDisplays", displays_tree);

    // COSMIC MODIFICATION: advertise the current wallpaper's content hash so a
    // client can skip re-fetching an unchanged image via GET /cosmic/wallpaper
    // (PLAN D10a/b). The hash itself leaks nothing; the image does, so it is
    // fetched separately over the client-certificate-verified HTTPS server.
    // The element is omitted entirely -- not emitted empty -- when there is no
    // wallpaper to share or wallpaper sharing is disabled; clients must treat
    // an absent CosmicWallpaperHash as "nothing to fetch".
    auto wallpaper_hash = cosmic::wallpaper::current_hash();
    if (!wallpaper_hash.empty()) {
      tree.put("root.CosmicWallpaperHash", wallpaper_hash);
    }

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args);

    if (rtsp_stream::session_count() == 0) {
      // The display should be restored in case something fails as there are no other sessions.
      revert_display_configuration = true;

      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // COSMIC MODIFICATION: record this connection's client certificate as
    // the clipboard owner now that this launch has succeeded (see the
    // comment above cosmic_clipboard_get further down).
    cosmic::clipboard::set_owner(cosmic_request_cert_fingerprint(request->remote_endpoint()));

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(host_audio, args);

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // COSMIC MODIFICATION: record this connection's client certificate as
    // the clipboard owner now that this resume has succeeded. A reconnect
    // (e.g. after a network blip) refreshes ownership to the reconnecting
    // client's certificate rather than dropping it -- see the comment above
    // cosmic_clipboard_get further down.
    cosmic::clipboard::set_owner(cosmic_request_cert_fingerprint(request->remote_endpoint()));
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  // COSMIC MODIFICATION: serve the host's desktop wallpaper (PLAN D10a/b,
  // milestone W1 item 2). This route only exists on this, the
  // client-certificate-verified HTTPS server -- never on http_server -- which
  // is the trust boundary that makes handing out the image acceptable at all.
  // The 8 MB size cap on the bytes is enforced inside
  // cosmic::wallpaper::read_bytes() itself, not here.
  void cosmic_wallpaper(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto bytes = cosmic::wallpaper::read_bytes();

    // Sniff the format from magic bytes rather than trusting a file
    // extension; read_bytes() only ever hands back the raw file. Anything
    // that doesn't match a known signature -- including "" for "no
    // wallpaper" or "sharing disabled" -- is never served.
    // COSMIC MODIFICATION: this signature list is duplicated in the provider
    // gate in src/hostglue/wallpaper.cpp (it only reports a hash/bytes for a
    // file matching one of these three signatures, so /serverinfo never
    // advertises a wallpaper this route cannot serve). Any change here must
    // update both call sites together.
    std::string_view content_type;
    if (bytes.size() >= 3 && (unsigned char) bytes[0] == 0xFF && (unsigned char) bytes[1] == 0xD8 && (unsigned char) bytes[2] == 0xFF) {
      content_type = "image/jpeg";
    } else if (bytes.size() >= 4 && (unsigned char) bytes[0] == 0x89 && (unsigned char) bytes[1] == 0x50 && (unsigned char) bytes[2] == 0x4E && (unsigned char) bytes[3] == 0x47) {
      content_type = "image/png";
    } else if (bytes.size() >= 2 && (unsigned char) bytes[0] == 0x42 && (unsigned char) bytes[1] == 0x4D) {
      content_type = "image/bmp";
    } else {
      // COSMIC MODIFICATION: not_found<SunshineHTTPS> is not used here -- it
      // calls response->write(data.str()), which resolves to the
      // string_view overload and emits an "HTTP/1.1 200 OK" status line with
      // Content-Length covering only its XML body; the 404-looking text it
      // writes afterward lands past Content-Length and is discarded. That is
      // fine for XML-consuming callers but wrong for a binary route, so this
      // route sends a real 404 status line instead.
      response->write(SimpleWeb::StatusCode::client_error_not_found);
      response->close_connection_after_response = true;
      return;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", content_type);
    // string_view overload writes the content-length header from bytes.size()
    // and streams exactly that many bytes, so embedded NUL bytes in the image
    // are not truncated (unlike a C-string based write).
    response->write(SimpleWeb::StatusCode::success_ok, bytes, headers);
    response->close_connection_after_response = true;
  }

  // COSMIC MODIFICATION: serve the host's local clipboard text to a
  // connected client. Like cosmic_wallpaper above, this route only exists on
  // this, the client-certificate-verified HTTPS server -- never on
  // http_server -- which is the trust boundary that makes handing out
  // clipboard contents acceptable at all.
  //
  // COSMIC MODIFICATION: the gate below adds a per-certificate owner check
  // on top of "any stream session is active" (rtsp_stream::session_count() >
  // 0). The owner is the SHA-256 fingerprint of the TLS client certificate
  // that authenticated the connection behind the most recent successful
  // /launch or /resume (cosmic::clipboard::set_owner, called from those
  // handlers); it is cleared (cosmic::clipboard::clear_owner, called
  // elsewhere) only when the LAST stream session on the host ends -- stream.cpp
  // gates the clear on `if (--running_sessions == 0)`, not on the owner's own
  // session ending. Consequence: with two concurrent sessions, a client that
  // recorded ownership and then disconnected keeps clipboard access for as
  // long as the other client is still streaming, because the counter never
  // reaches zero. A request whose connection's certificate does not match
  // the recorded owner -- or any request at all when no owner is recorded --
  // is answered 404, the same as if clipboard sharing were disabled or no
  // session were active (fail closed). Note that the `uniqueid` query
  // parameter is NOT used for this and is not an auth token: any paired
  // certificate can claim any uniqueid, so it cannot stand in for the
  // certificate that actually authenticated the connection.
  //
  // COSMIC MODIFICATION: this narrows the old "any paired client" hole but
  // does not close it. /resume does not verify that the requester is the
  // client that owns the already-running session: while client A streams,
  // a merely-paired client B can send its own GET /resume?rikey=...&rikeyid=
  // ...&corever=1 and reach launch_session_raise() -> set_owner(B's
  // fingerprint) without ever connecting over RTSP. Concretely,
  // current_appid != 0 passes because proc_t::running() (process.cpp:272)
  // reports an app as running either way: via the placebo flag when the
  // running app is the hardcoded Desktop stub (process.cpp:252), or via a
  // live child process for any real app (process.cpp:288) -- so the
  // seizure works whatever A is actually streaming, not only Desktop.
  // no_active_sessions is false so the display/encoder probe is skipped;
  // corever >= 1 makes launch_session->rtsp_cipher truthy so the
  // mandatory-encryption check is skipped; and make_launch_session()
  // performs no check that B is the client A started the session with. So a
  // paired client must now actively issue a /resume to take ownership --
  // it is no longer enough to merely be paired while someone else streams
  // -- but that is a narrower window, not an airtight boundary. Binding
  // ownership to the actual RTSP session (rather than to whichever paired
  // cert last called /launch or /resume) is a design change, tracked
  // separately.
  void cosmic_clipboard_get(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (
      !cosmic::clipboard::enabled() || rtsp_stream::session_count() == 0 ||
      // COSMIC MODIFICATION: only the client certificate that started the
      // active stream session may read the clipboard (see the comment
      // above).
      !cosmic::clipboard::is_owner(cosmic_request_cert_fingerprint(request->remote_endpoint()))
    ) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto since_str = get_arg(args, "since", "0");
    // std::strtoull never throws and returns 0 when it cannot parse any
    // leading digits, so a malformed "since" (missing, empty, non-numeric)
    // simply falls back to 0 rather than taking down the handler.
    std::uint64_t since = std::strtoull(since_str.c_str(), nullptr, 10);

    // COSMIC MODIFICATION: CosmicVersion 5 -- whether this client's Accept
    // header advertises image/png support. Captured by value below so a v1-4
    // client (no image/png in Accept) never receives PNG bytes labelled as
    // text/plain; see the mime checks in both branches below.
    bool accepts_png = cosmic_request_accepts_png(request->header);

    // COSMIC MODIFICATION: wait=1 long-polling (CosmicVersion 4). A client
    // that has nothing new normally polls once a second, paying one TLS
    // handshake per poll. When data is already available, fetch_or_park()
    // returns true and this falls through to the ordinary fetch below,
    // which responds exactly as it would without wait=1. Otherwise it parks
    // the response (via cb, captured by value) for up to kClipboardHoldMs
    // and returns without writing anything -- the callback completes the
    // response later, from whichever of publish()/tick()/clear_waiters()
    // first resolves this waiter.
    if (get_arg(args, "wait", "0") == "1") {
      bool available = cosmic::clipboard::fetch_or_park(
        since,
        cosmic::clipboard::kClipboardHoldMs,
        [response, accepts_png](cosmic::clipboard::WaitResult result, std::uint64_t wait_seq, cosmic::clipboard::Mime wait_mime, std::string wait_bytes) {
          if (result == cosmic::clipboard::WaitResult::Changed) {
            SimpleWeb::CaseInsensitiveMultimap wait_headers;
            wait_headers.emplace("X-Cosmic-Clipboard-Seq", std::to_string(wait_seq));
            // COSMIC MODIFICATION: CosmicVersion 5.
            wait_headers.emplace("X-Cosmic-Clipboard-Version", "5");
            if (wait_mime == cosmic::clipboard::Mime::Png && !accepts_png) {
              // COSMIC MODIFICATION: an old client cannot consume a PNG
              // payload -- answer 204 (seq advances past it) instead of
              // serving the image bytes as text/plain. Same
              // close-before-write ordering as the Timeout branch below.
              response->close_connection_after_response = true;
              response->write(SimpleWeb::StatusCode::success_no_content, wait_headers);
            } else {
              wait_headers.emplace("Content-Type", cosmic::clipboard::content_type(wait_mime));
              // string_view overload writes the content-length header from
              // wait_bytes.size() and streams exactly that many bytes, so
              // embedded NUL bytes in the clipboard text are not truncated
              // (unlike a C-string based write) -- same reasoning as
              // cosmic_wallpaper's write() above.
              response->write(SimpleWeb::StatusCode::success_ok, wait_bytes, wait_headers);
              response->close_connection_after_response = true;
            }
          } else if (result == cosmic::clipboard::WaitResult::Timeout) {
            SimpleWeb::CaseInsensitiveMultimap wait_headers;
            wait_headers.emplace("X-Cosmic-Clipboard-Seq", std::to_string(wait_seq));
            // COSMIC MODIFICATION: CosmicVersion 5.
            wait_headers.emplace("X-Cosmic-Clipboard-Version", "5");
            // RFC 7230 section 3.3.2 forbids a Content-Length header on a
            // 204 response. Simple-Web-Server's write_header()
            // (server_http.hpp) only skips emitting one when
            // close_connection_after_response is already true at write()
            // time, so the flag has to be set here, before write() -- same
            // reasoning as the immediate 204 path below.
            response->close_connection_after_response = true;
            response->write(SimpleWeb::StatusCode::success_no_content, wait_headers);
          } else {
            // Unavailable: clipboard sharing was turned off or the HTTPS
            // server is shutting down while this response was parked.
            response->write(SimpleWeb::StatusCode::client_error_not_found);
            response->close_connection_after_response = true;
          }
        }
      );
      if (!available) {
        return;
      }
    }

    std::string bytes;
    cosmic::clipboard::Mime mime = cosmic::clipboard::Mime::Text;
    std::uint64_t seq = 0;
    bool changed = cosmic::clipboard::fetch(since, bytes, mime, seq);

    // COSMIC MODIFICATION: cosmic::clipboard's sequence counter is
    // process-local and resets to 0 across a host restart. A client that
    // carries a "since" value from before the restart then has since > the
    // (now lower) current seq, and fetch() -- which only signals "changed"
    // when out_seq > since -- would otherwise report "no change" forever.
    // Treat a client cursor ahead of the store as stale rather than
    // starving it: re-fetch with since=0 to force the current text out,
    // but only when the store has actually been published to (seq > 0).
    // An unpublished store (seq == 0) has nothing to serve, so it falls
    // through to the ordinary 204 path below and the client resets its
    // cursor from the reported seq of 0.
    // Mirrored by the stale-cursor check in src/hostglue/clipboard.cpp's
    // fetch_or_park -- keep both predicates in sync.
    if (!changed && seq < since && seq > 0) {
      changed = cosmic::clipboard::fetch(0, bytes, mime, seq);
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("X-Cosmic-Clipboard-Seq", std::to_string(seq));
    // COSMIC MODIFICATION: CosmicVersion 5.
    headers.emplace("X-Cosmic-Clipboard-Version", "5");

    // COSMIC MODIFICATION: an old client cannot consume a PNG payload --
    // answer 204 (seq advances past it) instead of serving the image bytes
    // as text/plain. Same rule as the wait=1 Changed branch above.
    if (changed && mime == cosmic::clipboard::Mime::Png && !accepts_png) {
      changed = false;
    }

    if (changed) {
      headers.emplace("Content-Type", cosmic::clipboard::content_type(mime));
      // string_view overload writes the content-length header from
      // bytes.size() and streams exactly that many bytes, so embedded NUL
      // bytes in the clipboard text are not truncated (unlike a C-string
      // based write) -- same reasoning as cosmic_wallpaper's write() above.
      response->write(SimpleWeb::StatusCode::success_ok, bytes, headers);
    } else {
      // COSMIC MODIFICATION: RFC 7230 section 3.3.2 forbids a Content-Length
      // header on a 204 response. Simple-Web-Server's write_header()
      // (server_http.hpp) only skips emitting one when
      // close_connection_after_response is already true at write() time, so
      // the flag has to be set here, before write(), rather than by the
      // unconditional assignment below -- which still covers the 200 path
      // above, since this branch returns before reaching it.
      response->close_connection_after_response = true;
      response->write(SimpleWeb::StatusCode::success_no_content, headers);
      return;
    }
    response->close_connection_after_response = true;
  }

  // COSMIC MODIFICATION: accept a client's clipboard text and hand it off to
  // the app main thread via cosmic::clipboard::push_incoming(). Same
  // trust-boundary argument as cosmic_clipboard_get above. This is also the
  // first POST route this codebase registers.
  void cosmic_clipboard_post(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    if (
      !cosmic::clipboard::enabled() || rtsp_stream::session_count() == 0 ||
      // COSMIC MODIFICATION: only the client certificate that started the
      // active stream session may write the clipboard (see the comment
      // above cosmic_clipboard_get).
      !cosmic::clipboard::is_owner(cosmic_request_cert_fingerprint(request->remote_endpoint()))
    ) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
      response->close_connection_after_response = true;
      return;
    }

    // COSMIC MODIFICATION: CosmicVersion 5 -- classify the POST body by the
    // media type of its Content-Type header, ignoring any "; charset=..."
    // parameter and case. Absent header, or a literal text/plain, or
    // application/x-www-form-urlencoded (libcurl's default Content-Type for
    // CURLOPT_POSTFIELDS -- what a v1-4 client, which predates this route
    // distinguishing mime types, actually sends) are all treated as text.
    // Anything else is rejected: this route only ever stores text or PNG.
    cosmic::clipboard::Mime mime = cosmic::clipboard::Mime::Text;
    auto content_type_it = request->header.find("Content-Type");
    if (content_type_it != request->header.end()) {
      std::string media = content_type_it->second;
      auto semi = media.find(';');
      if (semi != std::string::npos) {
        media.resize(semi);
      }
      boost::algorithm::trim(media);
      if (boost::algorithm::iequals(media, "image/png"sv)) {
        mime = cosmic::clipboard::Mime::Png;
      } else if (!boost::algorithm::iequals(media, "text/plain"sv) &&
                 !boost::algorithm::iequals(media, "application/x-www-form-urlencoded"sv)) {
        response->write(SimpleWeb::StatusCode::client_error_unsupported_media_type);
        response->close_connection_after_response = true;
        return;
      }
    }

    // COSMIC MODIFICATION: Simple-Web-Server has already buffered the entire
    // body into request->content by the time this handler runs --
    // config.max_request_streambuf_size defaults to SIZE_MAX and is
    // server-wide, so it cannot be tightened for this route alone without
    // also capping /launch -- so this rejects an oversized body after it has
    // been buffered rather than before. Acceptable only because the route
    // sits behind client-certificate verification. Never truncate: reject
    // outright instead.
    if (request->content.size() > cosmic::clipboard::max_bytes(mime)) {
      response->write(SimpleWeb::StatusCode::client_error_payload_too_large);
      response->close_connection_after_response = true;
      return;
    }

    cosmic::clipboard::push_incoming(mime, request->content.string());
    response->write(SimpleWeb::StatusCode::success_ok);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  bool is_client_enabled(const std::string_view cert_pem);

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    // COSMIC MODIFICATION: added the `endpoint` parameter (see the comment
    // on SunshineHTTPSServer::verify above).
    https_server.verify = [add_cert](SSL *ssl, const boost::asio::ip::tcp::endpoint &endpoint) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      while (add_cert->peek()) {
        char subject_name[256];

        auto cert = add_cert->pop();
        X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << "Added cert ["sv << subject_name << ']';
        cert_chain.add(std::move(cert));
      }

      auto err_str = cert_chain.verify(x509.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      // Check if this client is enabled
      auto pem = crypto::pem(x509);
      if (!is_client_enabled(pem)) {
        BOOST_LOG(info) << "Client is disabled -- denied"sv;
        return verified;
      }

      verified = 1;

      // COSMIC MODIFICATION: remember which client certificate authenticated
      // this connection, so request handlers on this same connection can
      // later be gated to that specific certificate (see
      // cosmic_remember_client_cert above).
      cosmic_remember_client_cert(endpoint, cosmic_cert_fingerprint(x509.get()));

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    // COSMIC MODIFICATION: wallpaper route, HTTPS only (PLAN D10a/b).
    https_server.resource["^/cosmic/wallpaper$"]["GET"] = cosmic_wallpaper;
    // COSMIC MODIFICATION: clipboard routes, HTTPS only, same trust boundary
    // as /cosmic/wallpaper above. POST /cosmic/clipboard is the first POST
    // route this codebase registers on either server.
    https_server.resource["^/cosmic/clipboard$"]["GET"] = cosmic_clipboard_get;
    https_server.resource["^/cosmic/clipboard$"]["POST"] = cosmic_clipboard_post;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;

    // COSMIC MODIFICATION: Simple-Web-Server defaults thread_pool_size to 1,
    // so the server services a single connection at a time. A client that
    // holds an idle keep-alive connection -- which every Moonlight client does
    // for the length of a stream -- then occupies the only slot and no other
    // HTTPS request can even finish its TLS handshake. Give both servers room
    // so one parked connection cannot starve pairing, applist or launch.
    https_server.config.thread_pool_size = 4;
    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(add_cert, resp, req);
    };

    http_server.config.thread_pool_size = 4;
    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    // COSMIC MODIFICATION: resolve any GET /cosmic/clipboard?wait=1 request
    // still parked before the HTTPS server (and the io_context its Response
    // shared_ptrs are bound to) goes away underneath it.
    cosmic::clipboard::clear_waiters();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();

    // COSMIC MODIFICATION: a wait=1 request can be accepted and parked in
    // the window between the clear_waiters() above and the server actually
    // stopping; that Response then outlives https_server, and its shared_ptr
    // deleter posts send_on_delete to an io_context that is being destroyed.
    // Clear again now that ssl/tcp have joined, so anything parked in that
    // window is resolved while both server threads are provably done.
    // ACCEPTED residual gap: a waiter already extracted from g_waiters by a
    // concurrent tick()/publish() but not yet destroyed is not reachable by
    // either clear_waiters() call. Not closed here -- a shutdown-flag or
    // drain-barrier mechanism is disproportionate for this narrow a window.
    cosmic::clipboard::clear_waiters();
  }

  void erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
  }

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it).uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();
    return removed;
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      if (named_cert.uuid == uuid) {
        named_cert.enabled = enabled;
        save_state();
        return true;
      }
    }
    return false;
  }

  bool is_client_enabled(const std::string_view cert_pem) {
    const client_t &client = client_root;
    for (const auto &named_cert : client.named_devices) {
      if (named_cert.cert == cert_pem) {
        return named_cert.enabled;
      }
    }
    return true;
  }
}  // namespace nvhttp
