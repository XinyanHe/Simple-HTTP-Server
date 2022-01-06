#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <iostream>
#include <deque>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <thread>
#include <filesystem>
#include <chrono>

#define MAX_PENDING 5
#define MAX_LINE 1024

constexpr size_t BufferSize = 1024;

using namespace std;

using byte_t = char;

constexpr size_t EnqueueLimit = 0x10000;

// Date format for HTTP header is inspired from
// https://stackoverflow.com/questions/7548759/generate-a-date-string-in-http-response-date-format-in-c
string ConvertEpochToHTTPTime(long Time) {
  char buf[BufferSize];
  strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&Time));
  return string(buf);
}

// Get modify time of file specified by Path in epoch format
long GetModifyTime(string Path) {
  auto LWT = filesystem::last_write_time(filesystem::path(Path));
  return chrono::duration_cast<chrono::seconds>(LWT.time_since_epoch()).count();
}

// Get modify time in string format for file Path
string GetModifyTimeString(string Path) {
  return ConvertEpochToHTTPTime(GetModifyTime(Path));
}

// Convert HTTP date format to epoch time code is inspired from
// https://stackoverflow.com/questions/4781852/how-to-convert-a-string-to-datetime-in-c
long ConvertHTTPTimeToEpoch(string Time) {
  stringstream SS(Time);

  tm TM;

  SS >> get_time(&TM, "%a, %d %b %Y %H:%M:%S GMT");

  return mktime(&TM);
}

// Class to buffer HTTP requests
class EnqueueBuffer {
private:
  deque<byte_t> Buffer;
  size_t Limit = 0;

  size_t getRequestIndex() const {
    for (size_t i = 0; i + 3 < Buffer.size(); i++) {
      if (Buffer[i] == '\r' && Buffer[i + 1] == '\n'
          && Buffer[i + 2] == '\r' && Buffer[i + 3] == '\n')
        return i;
    }

    return SIZE_T_MAX;
  }

public:
  EnqueueBuffer(size_t Limit) {
    this->Limit = Limit;
  }

  void enqueue(byte_t *Data, size_t Size) {
    for (size_t i = 0; i < Size; i++) {
      Buffer.push_back(Data[i]);
    }
  }

  size_t size() const {
    return Buffer.size();
  }

  bool exceedLimit() const {
    return Buffer.size() > Limit;
  }

  bool hasRequest() const {
    return getRequestIndex() != SIZE_T_MAX;
  }

  string extractRequest() {
    assert(hasRequest());
    string Request;

    size_t Idx = getRequestIndex();

    for (size_t i = 0; i < Idx; i++) {
      Request.push_back(Buffer.front());
      Buffer.pop_front();
    }

    for (unsigned i = 0; i < 4; i++) {
      Buffer.pop_front();
    }

    return Request;
  }
};

// Class to parse request
class RequestParser {
private:
  bool Validity = true;
  string Resource;
  unordered_map<string, string> Header;

  string strim(string Str) const {
    size_t Idx = 0;
    while (Idx < Str.length() && Str[Idx] == ' ')
      Idx++;
    Str.erase(0, Idx);

    while (!Str.empty() && Str.back() == ' ')
      Str.pop_back();
    
    return Str;
  }

  // The string splitting function is written based on example from
  // https://coderedirect.com/questions/47718/parse-split-a-string-in-c-using-string-delimiter-standard-c
  vector<string> split(string Str, string Delimiter) const {
    size_t Pos = 0;
    vector<string> Ans;

    while ((Pos = Str.find(Delimiter)) != std::string::npos) {
      Ans.push_back(strim(Str.substr(0, Pos)));
      Str.erase(0, Pos + Delimiter.length());
    }

    Str = strim(Str);

    if (!Str.empty())
      Ans.push_back(Str);

    return Ans;
  }
public:
  RequestParser(string Req) {
    auto Lines = split(Req, "\r\n");

    if (Lines.empty()) {
      Validity = false;
      return;
    }

    // Check first line
    auto Request = split(Lines.front(), " ");
    if (Request.size() != 3) {
      Validity = false;
      return;
    }

    if (Request[0] != "GET" ||
        (Request[2] != "HTTP/1.0" && Request[2] != "HTTP/1.1")) {
      Validity = false;
      return;
    }

    Resource = Request[1];

    for (unsigned i = 1; i < Lines.size(); i++) {
      auto KV = split(Lines[i], ": ");
      if (KV.size() != 2) {
        Validity = false;
        return;
      }

      Header[KV.front()] = KV.back();
    }
  }

  bool isValid() const {
    return true;
  }

  string getResource() const {
    return Resource;
  }

  string getAttribute(string K) const {
    auto It = Header.find(K);
    return It == Header.end() ? "" : It->second;
  }
};

enum HTTPStatus {
  OK,
  NOT_FOUND,
  NOT_MODIFIED
};

enum MIMEType {
  HTML,
  TEXT,
  CSS,
  JS,
  JPEG
};

// Map http status to their response string
static const unordered_map<HTTPStatus, string> HTTPStatusMapping = {
  {OK, "200 OK"},
  {NOT_FOUND, "404 Not Found"},
  {NOT_MODIFIED, "304 Not Modified"}
};

// Map mime type to their response string
static const unordered_map<MIMEType, string> MIMETypeMapping = {
  {HTML, "text/html"},
  {TEXT, "text/plain"},
  {CSS, "text/css"},
  {JS, "text/javascript"},
  {JPEG, "image/jpeg"}
};

// Class to generate response string
class ResponseGenerator {
private:
  HTTPStatus Status;
  unordered_map<string, string> Header;

  string Body;
public:
  void setStatus(HTTPStatus Status) {
    this->Status = Status;
  }

  void setAttribute(string K, string V) {
    Header[K] = V;
  }

  void setBody(string Body) {
    this->Body = Body;
  }

  string getResponse() const {
    string Ans;

    Ans += "HTTP/1.1 " + HTTPStatusMapping.find(Status)->second + "\r\n";
    for (const auto [K, V] : Header) {
      Ans += K + ": " + V + "\r\n";
    }

    Ans += "\r\n" + Body;

    return Ans;
  }
};

// Server instance
class Server {
private:
  unsigned short Port;
  int Socket;
  string RootPath;

  pair<string, bool> readFile(string Path) const {
    ifstream IF(RootPath + Path);

    if (IF.fail())
      return {"", false};

    stringstream SS;
    SS << IF.rdbuf();
      
    return {SS.str(), true};
  }

  bool hasFile(string Path) const {
    ifstream IF(RootPath + Path);

    return !IF.fail();
  }

  bool endsWith(string Str, string Ending) const {
    if (Str.size() < Ending.size())
      return false;
    
    for (auto Sit = Str.crbegin(), Eit = Ending.crbegin(); Eit != Ending.crend(); Sit++, Eit++) {
      if (*Sit != *Eit)
        return false;
    }

    return true;
  }

  MIMEType getMIMEType(string FileName) const {
    if (endsWith(FileName, ".html"))
      return HTML;
    if (endsWith(FileName, ".css"))
      return CSS;
    if (endsWith(FileName, ".js"))
      return JS;
    if (endsWith(FileName, ".jpg") || endsWith(FileName, ".jpeg"))
      return JPEG;
    
    return TEXT;
  }

  // Given port, setup server connections and return the
  // listening socket
  void prepareServer() {
    struct sockaddr_in Sin;

    // Build address data structure
    bzero((char *)&Sin, sizeof(Sin));
    Sin.sin_family = AF_INET;
    Sin.sin_addr.s_addr = INADDR_ANY;
    Sin.sin_port = htons(Port);
    
    // Setup passive open
    if ((Socket = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
      perror("socket");
      exit(1);
    }

    // Bind socket to address
    if ((::bind(Socket, (struct sockaddr *)&Sin, sizeof(Sin))) < 0) {
      perror("bind");
      exit(1);
    }

    // Start listening
    listen(Socket, MAX_PENDING);
  }

  void runServer() {
    int ClientSocket;

    while (true) {
      if ((ClientSocket = accept(Socket, nullptr, nullptr)) < 0) {
        perror("accept");
        exit(1);
      }

      thread ClientHandler([&](int ClientSocket, string RootPath) {
        byte_t Message[MAX_LINE];
        unsigned MessageLength;

        EnqueueBuffer Enq(EnqueueLimit);

        while ((MessageLength = recv(ClientSocket, Message, MAX_LINE, 0))) {
          Enq.enqueue(Message, MessageLength);

          // Process request once receive a complete request
          if (Enq.hasRequest()) {
            string Req = Enq.extractRequest();
            cout << Req << endl;
            RequestParser ReqParser(Req);

            // Ignore invalid request
            if (!ReqParser.isValid())
              break;

            ResponseGenerator ResGenerator;

            if (hasFile(ReqParser.getResource())) {
              // If if-modified-since is there and it does satisfy
              if (!ReqParser.getAttribute("If-Modified-Since").empty() &&
                  GetModifyTime(RootPath + ReqParser.getResource()) <=
                  ConvertHTTPTimeToEpoch(ReqParser.getAttribute("If-Modified-Since"))) {
                ResGenerator.setStatus(NOT_MODIFIED);
                ResGenerator.setAttribute("Content-Type", MIMETypeMapping.find(TEXT)->second);
              } else {
                auto Resource = readFile(ReqParser.getResource());
                ResGenerator.setStatus(OK);
                ResGenerator.setAttribute("Content-Type", MIMETypeMapping.find(getMIMEType(ReqParser.getResource()))->second);
                ResGenerator.setBody(Resource.first);
              }
              ResGenerator.setAttribute("Last-Modified", GetModifyTimeString(RootPath + ReqParser.getResource()));
            } else {
              ResGenerator.setStatus(NOT_FOUND);
            }

            auto Res = ResGenerator.getResponse();

            cout << Res << endl;

            send(ClientSocket, Res.c_str(), Res.size(), 0);
            break;
          }
        }

        close(ClientSocket);
      }, ClientSocket, RootPath);

      ClientHandler.detach();
    }
  }

public:
  Server(unsigned short Port, string RootPath) {
    this->Port = Port;
    this->RootPath = RootPath;
  }

  void run() {
    prepareServer();
    runServer();
  }
};

int main(int argc, char **argv) {
  if (argc != 3) {
    cerr << argv[0] << ": [port] [root_path]" << endl;
    return 1;
  }

  // Obtain port number from command line arguments
  unsigned short Port = atoi(argv[1]);
  string RootPath(argv[2]);

  Server SimpleServer(Port, RootPath);

  SimpleServer.run();

  return 0;
}
