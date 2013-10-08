#include <thread>
#include <chrono>
#include <stdexcept>
#include <cctype>
#include <algorithm>
#include <vector>
#include <map>
#include <sstream>

#include "ogrsf_frmts.h"

#include "common/config.h"
#include "common/stringutils.h"
#include "server/worker.h"

using namespace Batyr;

struct OgrField 
{
    std::string name;
    unsigned int index;
    OGRFieldType type;
};
typedef std::map<std::string, OgrField> OgrFieldMap;


Worker::Worker(Configuration::Ptr _configuration, std::shared_ptr<JobStorage> _jobs)
    :   logger(Poco::Logger::get("Worker")),
        configuration(_configuration),
        jobs(_jobs),
        db(_configuration)
{
    poco_debug(logger, "Creating Worker");
}


Worker::~Worker()
{
    poco_debug(logger, "Destroying Worker");
}


void
Worker::pull(Job::Ptr job)
{
    if (job->getFilter().empty()) {
        poco_information(logger, "pulling layer \""+job->getLayerName()+"\"");
    }
    else {
        poco_information(logger, "pulling layer \""+job->getLayerName()+"\" using filter \""+job->getFilter()+"\"");
    }

    auto layer = configuration->getLayer(job->getLayerName());

    // open the dataset
    std::unique_ptr<OGRDataSource, void (*)(OGRDataSource*)> ogrDataset(
        OGRSFDriverRegistrar::Open(layer->source.c_str(), false),  OGRDataSource::DestroyDataSource);
    if (!ogrDataset) {
        throw WorkerError("Could not open dataset for layer \"" + layer->name + "\"");
    }

    // find the layer
    auto ogrLayer = ogrDataset->GetLayerByName(layer->source_layer.c_str());
    if (ogrLayer == nullptr) {
        throw WorkerError("source_layer \"" +layer->source_layer+ "\" in dataset for layer \""
                                + layer->name + "\" not found");
    }
    ogrLayer->ResetReading();

    // set filter if set
    std::string filterString = job->getFilter();
    if (!filterString.empty()) {
        CPLErrorReset();
        if (ogrLayer->SetAttributeFilter(filterString.c_str()) != OGRERR_NONE) {
            std::stringstream msgstream;
            msgstream   << "The given filter for layer \""
                        << layer->name
                        << "\" is invalid";
            if (CPLGetLastErrorMsg()) {
                msgstream   << ": " << CPLGetLastErrorMsg();
            }
            else {
                msgstream   << ".";
            }
            msgstream   << " The applied filter was [ "
                        << filterString
                        << " ]";
            CPLErrorReset();
            throw WorkerError(msgstream.str());
        }
    }
    
    auto ogrFeatureDefn = ogrLayer->GetLayerDefn();

#if GDAL_VERSION_MAJOR > 1
    if (ogrFeatureDefn->GetGeomFieldCount() != 1) {
        std::string msg = "The source provides " + std::to_string(ogrFeatureDefn->GetGeomFieldCount()) +
                "geometry fields. Currently only sources with on geoemtry field are supported";
        throw WorkerError(msg);
    }
#endif

    // collect the columns of the dataset
    OgrFieldMap ogrFields;
    for(int i=0; i<ogrFeatureDefn->GetFieldCount(); i++) {
        auto ogrFieldDefn = ogrFeatureDefn->GetFieldDefn(i);
        
        // lowercase column names -- TODO: this may cause problems when postgresqls column names
        // contain uppercase letters, but will do for a start
        std::string fieldNameCased = std::string(ogrFieldDefn->GetNameRef());
        std::string fieldName;
        std::transform(fieldNameCased.begin(), fieldNameCased.end(), std::back_inserter(fieldName), ::tolower);

#ifdef _DEBUG
        {
            std::string msg = "ogr layer provides the column " + fieldName;
            poco_debug(logger, msg.c_str());
        }
#endif
        auto entry = &ogrFields[fieldName];
        entry->index = i;
        entry->type = ogrFieldDefn->GetType();
    }

    // perform the work in an transaction
    if (auto transaction = db.getTransaction()) {
        int numPulled = 0;
        int numCreated = 0;
        int numUpdated = 0;
        int numDeleted = 0;

        // build a unique name for the temporary table
        std::string tempTableName = "batyr_" + job->getId();

        // create a temp table to write the data to
        transaction->createTempTable(layer->target_table_schema, layer->target_table_name, tempTableName);

        // fetch the column list from the target_table as the tempTable
        // does not have the constraints of the original table
        auto tableFields = transaction->getTableFields(layer->target_table_schema, layer->target_table_name);

        // check if the requirements of the primary key are satisfied
        // TODO: allow overriding the primarykey from the configfile
        std::vector<std::string> primaryKeyColumns;
        std::string geometryColumn;
        std::vector<std::string> insertColumns;
        std::vector<std::string> updateColumns;
        for(auto &tableFieldPair : tableFields) {
            if (tableFieldPair.second.isPrimaryKey) {
                primaryKeyColumns.push_back(tableFieldPair.second.name);
            }
            else {
                updateColumns.push_back(tableFieldPair.second.name);
            }
            if (tableFieldPair.second.pgTypeName == "geometry") {
                if (!geometryColumn.empty()) {
                    throw WorkerError("Layer \"" + job->getLayerName() + "\" has multiple geometry columns. Currently only one is supported");
                }
                geometryColumn = tableFieldPair.second.name;
                insertColumns.push_back(tableFieldPair.second.name);
            }
            if (ogrFields.find(tableFieldPair.second.name) != ogrFields.end()) {
                insertColumns.push_back(tableFieldPair.second.name);
            }
        }
        if (primaryKeyColumns.empty()) {
            throw WorkerError("Got no primarykey for layer \"" + job->getLayerName() + "\"");
        }
        std::vector<std::string> missingPrimaryKeysSource;
        for( auto &primaryKeyCol : primaryKeyColumns) {
            if (ogrFields.find(primaryKeyCol) == ogrFields.end()) {
                missingPrimaryKeysSource.push_back(primaryKeyCol);
            }
        }
        if (!missingPrimaryKeysSource.empty()) {
            throw WorkerError("The source for layer \"" + job->getLayerName() + "\" is missing the following fields required "+
                    "by the primary key: " + StringUtils::join(missingPrimaryKeysSource, ", "));
        }
        
        // prepare an insert query into the temporary table 
        std::vector<std::string> insertQueryValues;
        unsigned int idxColumn = 1;
        for (std::string &insertColumn : insertColumns) {
            std::stringstream colStream;
            colStream << "$" << idxColumn << "::" << tableFields[insertColumn].pgTypeName;
            insertQueryValues.push_back(colStream.str());
            idxColumn++;
        }
        std::stringstream insertQueryStream;
        // TODO: include st_transform statement into insert if original table has a srid set in geometry_columns
        insertQueryStream   << "insert into \"" << tempTableName << "\" (\""
                            << StringUtils::join(insertColumns, "\", \"")
                            << "\") values ("
                            << StringUtils::join(insertQueryValues, ", ")
                            << ")";
        poco_debug(logger, insertQueryStream.str().c_str());
        std::string insertStmtName = "batyr_insert" + job->getId();
        auto resInsertStmt = transaction->prepare(insertStmtName, insertQueryStream.str(), insertColumns.size(), NULL);

        OGRFeature * ogrFeature = 0;
        while( (ogrFeature = ogrLayer->GetNextFeature()) != nullptr) {
            std::vector<std::string> strValues;

            for (std::string &insertColumn : insertColumns) {
                auto tableField = &tableFields[insertColumn];

                if (tableField->pgTypeName == "geometry") {
                    // TODO: Maybe use the implementation from OGRPGLayer::GeometryToHex
                    GByte * buffer;

                    auto ogrGeometry = ogrFeature->GetGeometryRef();
                    int bufferSize = ogrGeometry->WkbSize();

                    buffer = (GByte *) CPLMalloc(bufferSize);
                    if (buffer == nullptr) {
                        throw WorkerError("Unable to allocate memory to export geometry");
                    }
                    if (ogrGeometry->exportToWkb(wkbNDR, buffer) != OGRERR_NONE) {
                        OGRFree(buffer);
                        throw WorkerError("Could not export the geometry from feature #" + std::to_string(numPulled));
                    }
                    char * hexBuffer = CPLBinaryToHex(bufferSize, buffer);
                    if (hexBuffer == nullptr) {
                        OGRFree(buffer);
                        throw WorkerError("Unable to allocate memory to convert geometry to hex");
                    }
                    OGRFree(buffer);
                    strValues.push_back(std::string(hexBuffer));
                    CPLFree(hexBuffer);
                }
                else {
                    auto ogrField = &ogrFields[insertColumn];
                    switch (ogrField->type) {
                        case OFTString:
                            strValues.push_back(std::string(ogrFeature->GetFieldAsString(ogrField->index)));
                            break; 
                        case OFTInteger:
                            strValues.push_back(std::to_string(ogrFeature->GetFieldAsInteger(ogrField->index)));
                            break;
                        case OFTReal:
                            strValues.push_back(std::to_string(ogrFeature->GetFieldAsDouble(ogrField->index)));
                            break;
                        // TODO: implment all of the OGRFieldType types
                        default:
                            throw WorkerError("Unsupported OGR field type: " + std::to_string(static_cast<int>(ogrField->type)));
                    }
                }
            }


            // convert to an array of c strings
            std::vector<const char*> cStrValues;
            std::vector<int> cStrValueLenghts;
            std::transform(strValues.begin(), strValues.end(), std::back_inserter(cStrValues),
                        [](std::string & s){ return s.c_str();});
            std::transform(strValues.begin(), strValues.end(), std::back_inserter(cStrValueLenghts),
                        [](std::string & s){ return s.length();});
            
            transaction->execPrepared(insertStmtName, cStrValues.size(), &cStrValues[0], &cStrValueLenghts[0],
                        NULL, 1);

            numPulled++;
        }
        job->setStatistics(numPulled, numCreated, numUpdated, numDeleted);

        // update the existing table only touching rows which have differences to prevent
        // slowdowns by triggers
        std::stringstream updateStmt;
        updateStmt          << "update \"" << layer->target_table_schema << "\".\"" << layer->target_table_name << "\" "
                            << " set ";
        for (size_t i=0; i<updateColumns.size(); i++) {
            if (i != 0) {
                updateStmt << ", ";
            }
            updateStmt << "\"" << updateColumns[i] << "\" = \"" << tempTableName << "\".\"" << updateColumns[i] << "\" ";
        }
        updateStmt          << " from \"" << tempTableName << "\""
                            << " where (";
        for (size_t i=0; i<primaryKeyColumns.size(); i++) {
            if (i != 0) {
                updateStmt << " and ";
            }
            updateStmt  << "\""  << layer->target_table_name << "\".\"" << primaryKeyColumns[i]
                        << "\" is not distinct from \"" << tempTableName << "\".\"" << primaryKeyColumns[i] << "\"";
        }
        updateStmt          << ") and (";
        // update only rows which are actual different
        for (size_t i=0; i<updateColumns.size(); i++) {
            if (i != 0) {
                updateStmt << " or ";
            }
            updateStmt  << "(\"" << layer->target_table_name << "\".\"" << updateColumns[i]
                        << "\" is distinct from  \"" 
                        << tempTableName << "\".\"" << updateColumns[i] << "\")";
        }
        updateStmt          << ")";
        auto updateRes = transaction->exec(updateStmt.str());
        numUpdated = std::atoi(PQcmdTuples(updateRes.get()));
        updateRes.reset(NULL); // immediately dispose the result

        // insert missing rows in the exisiting table
        std::stringstream insertMissingStmt;
        insertMissingStmt   << "insert into \"" << layer->target_table_schema << "\".\"" << layer->target_table_name << "\" "
                            << " ( \"" << StringUtils::join(insertColumns, "\", \"") << "\") "
                            << " select \"" << StringUtils::join(insertColumns, "\", \"") << "\" "
                            << " from \"" << tempTableName << "\""
                            << " where (\"" << StringUtils::join(primaryKeyColumns, "\", \"") << "\") not in ("
                            << " select \"" << StringUtils::join(primaryKeyColumns, "\",\"") << "\" "
                            << "       from \"" << layer->target_table_schema << "\".\""  << layer->target_table_name << "\""
                            << ")";
        auto insertMissingRes = transaction->exec(insertMissingStmt.str());
        numCreated = std::atoi(PQcmdTuples(insertMissingRes.get()));
        insertMissingRes.reset(NULL); // immediately dispose the result

        // delete deprecated rows from the exisiting table
        // TODO: make this optional and skip when a filter is used
        std::stringstream deleteRemovedStmt;
        deleteRemovedStmt   << "delete from \"" << layer->target_table_schema << "\".\"" << layer->target_table_name << "\" "
                            << " where (\"" << StringUtils::join(primaryKeyColumns, "\", \"") << "\") not in ("
                            << " select \"" << StringUtils::join(primaryKeyColumns, "\",\"") << "\" "
                            << "       from \"" << tempTableName << "\""
                            << ")";
        auto deleteRemovedRes = transaction->exec(deleteRemovedStmt.str());
        numDeleted = std::atoi(PQcmdTuples(deleteRemovedRes.get()));
        deleteRemovedRes.reset(NULL); // immediately dispose the result

        job->setStatus(Job::Status::FINISHED);
        job->setStatistics(numPulled, numCreated, numUpdated, numDeleted);
    }
    else {
        std::string msg("Could not start a database transaction");
        poco_error(logger, msg.c_str());
        job->setStatus(Job::Status::FAILED);
        job->setMessage(msg);
    }

}


void
Worker::run()
{
    while (true) {
        Job::Ptr job;
        try {
            bool got_job = jobs->pop(job);
            if (!got_job) {
                // no job means the queue recieved a quit command, so the worker
                // can be shut down
                break;
            }
            poco_debug(logger, "Got job from queue");

            job->setStatus(Job::Status::IN_PROCESS);

            // check if we got a working database connection
            // or block until we got one
            size_t reconnectAttempts = 0;
            while(!db.reconnect(true)) {
                if (reconnectAttempts == 0) {
                    // set job message to inform clients we are waiting here
                    job->setMessage("Waiting to aquire a database connection");
                }
                reconnectAttempts++;
                std::this_thread::sleep_for( std::chrono::milliseconds( SERVER_DB_RECONNECT_WAIT ) );
            }
            job->setMessage("");

            pull(job);
        }
        catch (Batyr::Db::DbError &e) {
            poco_error(logger, e.what());
            job->setStatus(Job::Status::FAILED);
            job->setMessage(e.what());
        }
        catch (WorkerError &e) {
            poco_error(logger, e.what());
            job->setStatus(Job::Status::FAILED);
            job->setMessage(e.what());
        }
        catch (std::runtime_error &e) {
            poco_error(logger, e.what());
            job->setStatus(Job::Status::FAILED);
            job->setMessage(e.what());

            // do not know how this exception was caused as it
            // was not handled by one of the earlier catch blocks
            throw;
        }
    }
    poco_debug(logger, "leaving run method");
}
