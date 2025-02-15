#include "Connection.hpp"

#include <unistd.h>

#include <cstring>
#include <iostream>

#include "../config/Location.hpp"
#include "../core/Kqueue.hpp"
#include "../utils/Config.hpp"

/**
 * Constructor & Destructor
 */

Connection::Connection(int fd, ServerManager& manager)
    : _fd(fd),
      _lastCallTime(std::time(0)),
      _status(ON_WAIT),
      _requestParser(),
      _responseBuilder(NULL),
      _manager(manager) {}

Connection::Connection(Connection const& connection)
    : _manager(connection._manager) {
  *this = connection;
}

Connection::~Connection(void) {}

/**
 * Operator Overloading
 */

Connection& Connection::operator=(Connection const& connection) {
  if (this != &connection) {
    _fd = connection._fd;
    _lastCallTime = connection._lastCallTime;
    _status = connection._status;
    _requestParser = connection._requestParser;
    _responseBuilder = connection._responseBuilder;
    _builderFds = connection._builderFds;
    _manager = connection._manager;
  }
  return *this;
}

/**
 * Public method - request
 */

// 요청 읽기
// - 읽기에 실패한 경우 예외 발생
// - 클라이언트 연결 종료가 감지된 경우 status를 CLOSE로 바꿈
void Connection::readSocket(void) {
  if (_status == ON_WAIT) {
    setStatus(ON_RECV);
  }

  octet_t buffer[BUFFER_SIZE];
  memset(buffer, 0, BUFFER_SIZE);
  ssize_t bytesRead = read(_fd, buffer, sizeof(buffer));

  if (bytesRead < 0) {
    throw std::runtime_error("[4000] Connection: readSocket - read fail");
  }

  if (bytesRead == 0) {  // 클라이언트가 연결을 종료했음
    setStatus(CLOSE);
    std::cout << "Client: connection closed" << std::endl;  // debug
    updateLastCallTime();
    return;
  }

  parseRequest(buffer, bytesRead);
  updateLastCallTime();
}

// storage에 있는 요청 읽기
// - 읽기에 실패한 경우 예외 발생
void Connection::readStorage(void) {
  if (_status == ON_WAIT) {
    setStatus(ON_RECV);
  }

  octet_t tmp[1];

  parseRequest(tmp, 0);
  updateLastCallTime();
}

// RequestParser에서 요청 읽기
// - bytesRead를 0으로 하면 storage에 남아있는 내용을 파싱
void Connection::parseRequest(octet_t const* buffer, ssize_t bytesRead) {
  _requestParser.parse(buffer, bytesRead);

  // 헤더 읽기 완료
  if (_requestParser.getParsingStatus() == HEADER_FIELD_END) {
    // Location 블록 할당
    Request const& request = _requestParser.getRequest();
    setRequestParserLocation(request);
    // 다시 파싱
    _requestParser.parse(buffer, 0);
  }

  // 전체 요청 읽기 완료
  if (_requestParser.getParsingStatus() == DONE) {
    setStatus(TO_SEND);

    std::cout << "------------------------------------\n"
              << "[ Server: request received ]\n"
              << "------------------------------------\n";

    // debug
    Request const& request = _requestParser.getRequest();
    request.print();
  }
}

// RequestParser의 storage가 남아있는지 여부 반환
// - storage가 남아있는 경우 readStorage 메서드를 호출해야 함
bool Connection::isReadStorageRequired() {
  return _requestParser.isStorageBufferNotEmpty();
}

/**
 * Public method - response
 */

// HTTP 요청 + Location 블록을 보고 분기
// - 적절한 ResponseBuilder 선택
void Connection::selectResponseBuilder(void) {
  updateLastCallTime();

  Request const& request = _requestParser.getRequest();
  Location const& location = request.getLocation();

  if (location.isAllowMethod(request.getMethod()) == false) {
    throw StatusException(
        HTTP_NOT_ALLOWED,
        "[4005] Connection: selectResponseBuilder - method not allowed");
  }

  _responseBuilder = BuilderSelector::getMatchingBuilder(request);
  setStatus(ON_BUILD);
}

// HTTP 응답 만들기
void Connection::buildResponse(Event::EventType eventType) {
  std::vector<int> const& builderFds = _responseBuilder->build(eventType);
  updateLastCallTime();

  if (builderFds.size() != 0) {
    for (std::vector<int>::const_iterator it = builderFds.begin();
         it != builderFds.end(); ++it) {
      _manager.addManagedFd(*it, _fd);
    }
    _builderFds = builderFds;
  }

  if (_responseBuilder->isDone()) {
    setStatus(ON_SEND);
    _responseBuilder->close();
    removeAllBuilderFd();
  }
}

// 응답 전송
void Connection::sendResponse(void) {
  bool isDone = false;
  Response& response = _responseBuilder->getResponse();
  std::string const& responseContent = response.toString();

  size_t startIndex = response.getStartIndex();
  size_t contentLength = responseContent.length() - startIndex;

  std::string sendString;
  if (BUFFER_SIZE < contentLength) {
    sendString = responseContent.substr(startIndex, BUFFER_SIZE);
  } else {
    sendString = responseContent.substr(startIndex);
    isDone = true;
  }

  ssize_t bytesSent;
  bytesSent = write(_fd, sendString.c_str(), sendString.length());
  if (bytesSent < 0) {
    throw std::runtime_error(
        "[4002] Connection: sendErrorPage - fail to write socket");
  }

  response.setStartIndex(startIndex + bytesSent);
  updateLastCallTime();

  if (isDone) {
    std::cout << "------------------------------------\n"
              << "[ Server: response sent ]\n"
              << "------------------------------------\n";

    response.print();

    _responseBuilder->isConnectionClose() ? setStatus(CLOSE)
                                          : setStatus(ON_WAIT);
  }
}

void Connection::resetResponseBuilder(int code) {
  updateLastCallTime();

  Kqueue::removeAllEvents(_fd);

  removeAllBuilderFd();

  if (_responseBuilder != NULL) {
    delete _responseBuilder;
    _responseBuilder = NULL;
  }

  _responseBuilder = new ErrorBuilder(_requestParser.getRequest(), code);
  setStatus(ON_BUILD);

  buildResponse(Event::NONE);
  if (_status == Connection::ON_SEND) {
    Kqueue::addWriteEvent(_fd);
  }
}

void Connection::resetResponseBuilder(void) {
  updateLastCallTime();

  Kqueue::removeAllEvents(_fd);

  removeAllBuilderFd();

  if (_responseBuilder != NULL) {
    delete _responseBuilder;
    _responseBuilder = NULL;
  }

  _responseBuilder = new ErrorBuilder();
  setStatus(ON_BUILD);

  buildResponse(Event::NONE);
  if (_status == Connection::ON_SEND) {
    Kqueue::addWriteEvent(_fd);
  }
}

/**
 * Public method - etc
 */

void Connection::clear(void) {
  _requestParser.clear();
  if (_responseBuilder != NULL) {
    delete _responseBuilder;
    _responseBuilder = NULL;
  }
}

// 커넥션 닫기
void Connection::close(void) { ::close(_fd); }

// 관리 중인 fd 반환
int Connection::getFd(void) const { return _fd; }

// 현재 커넥션의 상태 반환
Connection::EStatus Connection::getConnectionStatus(void) const {
  return _status;
}

// 마지막으로 호출된 이후 경과된 시간 반환
// - 반환된 값은 초 단위
long Connection::getElapsedTime(void) const {
  return std::time(0) - _lastCallTime;
}

// Connection의 현재 상태가 파라미터 상태와 동일한지 확인
bool Connection::isSameState(EStatus status) { return (_status == status); }

/**
 * Private method
 */

// path와 host 정보를 가지고 알맞은 location 블럭을 할당
void Connection::setRequestParserLocation(Request const& request) {
  std::string const& path = request.getPath();
  std::string const host = request.getHost();

  Location const& location = _manager.getLocation(path, host);
  _requestParser.initRequestLocationAndFullPath(location);
}

// builder에서 사용하는 모든 fd 정보를 제거
void Connection::removeAllBuilderFd(void) {
  if (_builderFds.size() != 0) {
    for (std::vector<int>::const_iterator it = _builderFds.begin();
         it != _builderFds.end(); ++it) {
      _manager.removeManagedFd(*it);
    }
    _builderFds.clear();
  }
}

// 마지막으로 호출된 시간 업데이트
// - timeout 관리를 위해 커넥션이 호출되면 반드시 사용
void Connection::updateLastCallTime(void) { _lastCallTime = std::time(0); }

// Connection의 현재 상태 변경
void Connection::setStatus(EStatus status) { _status = status; }
