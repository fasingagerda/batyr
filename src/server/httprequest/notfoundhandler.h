#ifndef __batyr_httprequest_notfoundhandler_h__
#define __batyr_httprequest_notfoundhandler_h__


#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>


namespace Batyr 
{
namespace HttpRequest 
{

    class NotFoundHandler : public Poco::Net::HTTPRequestHandler
    {
        public:
            virtual void handleRequest(Poco::Net::HTTPServerRequest &req, Poco::Net::HTTPServerResponse &resp);
    };

};
};

#endif // __batyr_httprequest_notfoundhandler_h__