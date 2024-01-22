#ifndef __A_RESPONSE_BUILDER_HPP__
#define __A_RESPONSE_BUILDER_HPP__

#include "Request.hpp"
#include "Response.hpp"

class AResponseBuilder {
 public:
  enum EBuilderType { ERROR };

 protected:
  Response _response;
  bool _isDone;

 private:
  Request _request;
  EBuilderType _type;

 public:
  AResponseBuilder(EBuilderType const& type, Request const& request);
  AResponseBuilder(AResponseBuilder const& builder);
  virtual ~AResponseBuilder(void);

  AResponseBuilder& operator=(AResponseBuilder const& builder);

  EBuilderType const& getType(void) const;
  Response const& getResponse(void) const;
  bool isDone(void) const;

  virtual void build(void) = 0;
  virtual void close(void) = 0;

 protected:
  Request const& getRequest(void) const;

  void setRequest(Request const& request);
  void setType(EBuilderType const& type);

 private:
  AResponseBuilder(void);
};

#endif