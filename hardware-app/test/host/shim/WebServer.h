// A real HTTP server behind Arduino's WebServer API, so src/portal.cpp can be
// driven by a real browser on the host.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"

typedef enum { HTTP_ANY, HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE } HTTPMethod;

class WebServer {
 public:
  using Handler = std::function<void()>;

  explicit WebServer(int port = 80);
  ~WebServer();

  void on(const char* uri, HTTPMethod method, Handler handler);
  void on(const char* uri, Handler handler) { on(uri, HTTP_ANY, std::move(handler)); }
  void onNotFound(Handler handler) { notFound_ = std::move(handler); }
  void collectHeaders(const char** headers, size_t count);

  void begin();
  /** Serve at most one request; returns immediately when none is waiting. */
  void handleClient();

  void send(int code, const char* contentType, const String& body);
  void send(int code, const char* contentType, const char* body);
  void send_P(int code, const char* contentType, const char* body);
  void sendHeader(const String& name, const String& value, bool first = false);

  String arg(const char* name) const;
  bool hasHeader(const char* name) const;
  String header(const char* name) const;
  String uri() const { return String(uri_); }
  HTTPMethod method() const { return method_; }

  /** The port actually bound (TARDIS_PORTAL_PORT overrides port 80 on a host
   *  where binding 80 is not allowed). */
  int boundPort() const { return port_; }

 private:
  struct Route {
    std::string uri;
    HTTPMethod method;
    Handler handler;
  };

  int port_;
  int listener_ = -1;
  int client_ = -1;
  std::vector<Route> routes_;
  Handler notFound_;
  std::vector<std::string> collected_;

  std::string uri_;
  HTTPMethod method_ = HTTP_GET;
  std::string body_;
  std::map<std::string, std::string> headers_;
  std::vector<std::pair<std::string, std::string>> pendingHeaders_;
  bool responded_ = false;

  bool readRequest();
  void writeResponse(int code, const std::string& contentType, const std::string& body);
};
