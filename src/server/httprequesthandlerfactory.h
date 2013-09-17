#ifndef __batyr_httprequesthandlerfactory_h__
#define __batyr_httprequesthandlerfactory_h__

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include "Poco/Logger.h"

#include <memory>

#include "joblist.h"

namespace Batyr {

    class HTTPRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
    {
        public:
            HTTPRequestHandlerFactory();
            virtual Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &);

            void setJobs(std::weak_ptr<JobList> _jobs)
            {
                jobs = _jobs;
            }
            
        private:
            Poco::Logger & logger;
            std::weak_ptr<JobList> jobs;
    };
    

};

#endif // __batyr_httprequesthandlerfactory_h__
