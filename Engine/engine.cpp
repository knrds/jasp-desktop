//
// Copyright (C) 2013-2018 University of Amsterdam
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "engine.h"

#include <sstream>
#include <cstdio>

//#include "../Common/analysisloader.h"
#include <boost/bind.hpp>
#include "../Common/tempfiles.h"
#include "../Common/utils.h"
#include "../Common/sharedmemory.h"
#include <csignal>

#include "rbridge.h"
#include "timers.h"
#include "log.h"


void SendFunctionForJaspresults(const char * msg) { Engine::theEngine()->sendString(msg); }
bool PollMessagesFunctionForJaspResults()
{
	if(Engine::theEngine()->receiveMessages())
	{
		if(Engine::theEngine()->paused())
			return true;
		else
			switch(Engine::theEngine()->getAnalysisStatus())
			{
			case engineAnalysisStatus::changed:
			case engineAnalysisStatus::aborted:
			case engineAnalysisStatus::stopped:
				Log::log() << "Analysis status changed for engine #" << Engine::theEngine()->slaveNo() << " to: " << engineAnalysisStatusToString(Engine::theEngine()->getAnalysisStatus()) << std::endl;
				return true;
			default:							break;
			}
	}
	return false;
}

#ifdef _WIN32

#undef Realloc
#undef Free

#endif

Engine * Engine::_EngineInstance = NULL;

Engine::Engine(int slaveNo, unsigned long parentPID)
	: _slaveNo(slaveNo), _parentPID(parentPID)
{
	JASPTIMER_SCOPE(Engine Constructor);
	assert(_EngineInstance == NULL);
	_EngineInstance = this;

	JASPTIMER_START(TempFiles Attach);
	TempFiles::attach(parentPID);
	JASPTIMER_STOP(TempFiles Attach);

	rbridge_setDataSetSource(			boost::bind(&Engine::provideDataSet,				this));
	rbridge_setFileNameSource(			boost::bind(&Engine::provideTempFileName,			this, _1, _2, _3));
	rbridge_setSpecificFileNameSource(	boost::bind(&Engine::provideSpecificFileName,		this, _1, _2, _3));

	rbridge_setStateFileSource(			boost::bind(&Engine::provideStateFileName,			this, _1, _2));
	rbridge_setJaspResultsFileSource(	boost::bind(&Engine::provideJaspResultsFileName,	this, _1, _2));

	rbridge_setColumnFunctionSources(	boost::bind(&Engine::getColumnType,					this, _1),
										boost::bind(&Engine::setColumnDataAsScale,			this, _1, _2),
										boost::bind(&Engine::setColumnDataAsOrdinal,		this, _1, _2, _3),
										boost::bind(&Engine::setColumnDataAsNominal,		this, _1, _2, _3),
										boost::bind(&Engine::setColumnDataAsNominalText,	this, _1, _2));

	rbridge_setGetDataSetRowCountSource( boost::bind(&Engine::dataSetRowCount, this));
	
	_extraEncodings = new ColumnEncoder("JaspExtraOptions_.");

}

void Engine::initialize()
{
	Log::log() << "Engine::initialize()" << std::endl;

	try
	{
		rbridge_init(SendFunctionForJaspresults, PollMessagesFunctionForJaspResults, _extraEncodings);

		Log::log() << "rbridge_init completed" << std::endl;

#if defined(JASP_DEBUG) || defined(__linux__)
		if (_slaveNo == 0)
		{
			Log::log() << rbridge_check()			<< std::endl;
			Log::log() << "rbridge_check completed" << std::endl;
		}
#endif
	
		//Is there maybe already some data? Like, if we just killed and restarted the engine
		ColumnEncoder::columnEncoder()->setCurrentColumnNames(provideDataSet() == nullptr ? std::vector<std::string>({}) : provideDataSet()->getColumnNames());

		_engineState = engineState::idle;
		sendEngineResumed(); //Then the desktop knows we've finished init.

		Log::log() << "Engine::initialize() done" << std::endl;
	}
	catch(std::exception & e)
	{
		Log::log() << "Engine::initialize() failed! The exception caught was: '" << e.what() << "'" << std::endl;
		throw e;
	}
}

Engine::~Engine()
{
	TempFiles::deleteAll();

	delete _channel; //shared memory files will be removed in jaspDesktop
	_channel = nullptr;
}

void Engine::run()
{
	JASPTIMER_START(Engine::run startup);

	std::string memoryName = "JASP-IPC-" + std::to_string(_parentPID);
	_channel = new IPCChannel(memoryName, _slaveNo, true);

	JASPTIMER_STOP(Engine::run startup);

	sendString(""); //Clear the buffer, because it might have been filled by a previous incarnation of the engine

	while(_engineState != engineState::stopped && ProcessInfo::isParentRunning())
	{
		if(_engineState == engineState::initializing) //Do this first, otherwise receiveMessages possibly triggers some other functions
			initialize();

		receiveMessages(100);

		switch(_engineState)
		{
		case engineState::idle:				beIdle(_lastRequest == engineState::analysis);	break;
		case engineState::analysis:			runAnalysis();									break;
		case engineState::paused:			/* Do nothing */
		case engineState::stopped:															break;
		case engineState::resuming:			throw std::runtime_error("Enginestate " + engineStateToString(_engineState) + " should NOT be set as currentState!");
		default:
			Log::log() << "Engine got stuck in engineState " << engineStateToString(_engineState) << " which is not supposed to happen..." << std::endl;
		}
	}

	if(_engineState == engineState::stopped)
		Log::log() << "Engine leaving mainloop after having been asked to stop." << std::endl;
}

void Engine::beIdle(bool newlyIdle)
{
	static int idleStartTime = -1;

	if(newlyIdle)
		idleStartTime = Utils::currentSeconds();
	else if(idleStartTime != -1 && idleStartTime + 10 < Utils::currentSeconds())
	{
		Log::log() << "Attempting to clean up memory used by engine/R a bit." << std::endl;
		rbridge_memoryCleaning();
		idleStartTime = -1;
	}

	_lastRequest = engineState::idle;
}



bool Engine::receiveMessages(int timeout)
{
	std::string data;

	if (_channel->receive(data, timeout))
	{
		if(data =="")
			return false;



		Json::Value		jsonRequest;
		Json::Reader	jsonReader;

		if(!jsonReader.parse(data, jsonRequest, false))
		{
			Log::log() << "Engine got request:\nrow 0:\t";

			size_t row=0;
			for(const char & c : data)
			{
				if(c == '\n')
					Log::log() << "\nrow " << ++row << ":\t";
				Log::log() << c;
			}

			Log::log() << "Parsing request failed on:\n" << jsonReader.getFormatedErrorMessages() << std::endl;
		}

		//Clear send buffer
		sendString("");

		//Check if we got anyting useful
		std::string typeSend	= jsonRequest.get("typeRequest", Json::nullValue).asString();
		if(typeSend == "")
		{
			Log::log() << "It seems the required field \"typeRequest\" was empty: '" << typeSend << "'" << std::endl;
			return false;
		}

		_lastRequest = engineStateFromString(typeSend);

#ifdef PRINT_ENGINE_MESSAGES
		Log::log() << "Engine received " << engineStateToString(_lastRequest) <<" message" << std::endl;
#endif
		switch(_lastRequest)
		{
		case engineState::analysis:				receiveAnalysisMessage(jsonRequest);		return true;
		case engineState::filter:				receiveFilterMessage(jsonRequest);			break;
		case engineState::rCode:				receiveRCodeMessage(jsonRequest);			break;
		case engineState::computeColumn:		receiveComputeColumnMessage(jsonRequest);	break;
		case engineState::pauseRequested:		pauseEngine();								break;
		case engineState::resuming:				resumeEngine(jsonRequest);					break;
		case engineState::moduleInstallRequest:	
		case engineState::moduleLoadRequest:	receiveModuleRequestMessage(jsonRequest);	break;
		case engineState::stopRequested:		stopEngine();								break;
		case engineState::logCfg:				receiveLogCfg(jsonRequest);					break;
		case engineState::settings:				receiveSettings(jsonRequest);				break;
		default:								throw std::runtime_error("Engine::receiveMessages begs you to add your new engineState " + engineStateToString(_lastRequest) + " to it!");
		}
	}

	return false;
}

void Engine::receiveFilterMessage(const Json::Value & jsonRequest)
{
	if(_engineState != engineState::idle)
		Log::log() << "Unexpected filter message, current state is not idle (" << engineStateToString(_engineState) << ")";

	_engineState				= engineState::filter;
	std::string filter			= jsonRequest.get("filter", "").asString();
	std::string generatedFilter = jsonRequest.get("generatedFilter", "").asString();
	int filterRequestId			= jsonRequest.get("requestId", -1).asInt();

	runFilter(filter, generatedFilter, filterRequestId);
}

void Engine::runFilter(const std::string & filter, const std::string & generatedFilter, int filterRequestId)
{
	try
	{
        std::string strippedFilter		= stringUtils::stripRComments(filter);
		std::vector<bool> filterResult	= rbridge_applyFilter(strippedFilter, generatedFilter);
		std::string RPossibleWarning	= jaspRCPP_getLastErrorMsg();

		sendFilterResult(filterRequestId, filterResult, RPossibleWarning);

	}
	catch(filterException & e)
	{
		sendFilterError(filterRequestId, std::string(e.what()).length() > 0 ? e.what() : "Something went wrong with the filter but it is unclear what.");
	}

	_engineState = engineState::idle;
}

void Engine::sendFilterResult(int filterRequestId, const std::vector<bool> & filterResult, const std::string & warning)
{
	Json::Value filterResponse(Json::objectValue);

	filterResponse["typeRequest"]	= engineStateToString(engineState::filter);
	filterResponse["filterResult"]	= Json::arrayValue;
	filterResponse["requestId"]		= filterRequestId;

	for(bool f : filterResult)	filterResponse["filterResult"].append(f);
	if(warning != "")			filterResponse["filterError"] = warning;

	sendString(filterResponse.toStyledString());
}

void Engine::sendFilterError(int filterRequestId, const std::string & errorMessage)
{
	Json::Value filterResponse = Json::Value(Json::objectValue);

	filterResponse["typeRequest"]	= engineStateToString(engineState::filter);
	filterResponse["filterError"]	= errorMessage;
	filterResponse["requestId"]		= filterRequestId;

	sendString(filterResponse.toStyledString());
}

void Engine::receiveRCodeMessage(const Json::Value & jsonRequest)
{
	if(_engineState != engineState::idle)
		Log::log() << "Unexpected rCode message, current state is not idle (" << engineStateToString(_engineState) << ")";

				_engineState	= engineState::rCode;
	std::string rCode			= jsonRequest.get("rCode",			"").asString();
	int			rCodeRequestId	= jsonRequest.get("requestId",		-1).asInt();
	bool		whiteListed		= jsonRequest.get("whiteListed",	true).asBool(),
	            returnLog		= jsonRequest.get("returnLog",		false).asBool();

	if(returnLog)	runRCodeCommander(rCode);
	else			runRCode(rCode, rCodeRequestId, whiteListed);
}

// Evaluating arbitrary R code (as string) which returns a string
void Engine::runRCode(const std::string & rCode, int rCodeRequestId, bool whiteListed)
{

	std::string rCodeResult = whiteListed ? rbridge_evalRCodeWhiteListed(rCode.c_str()) : jaspRCPP_evalRCode(rCode.c_str());

	if (rCodeResult == "null")	sendRCodeError(rCodeRequestId);
	else						sendRCodeResult(rCodeResult, rCodeRequestId);

	_engineState = engineState::idle;
}


void Engine::runRCodeCommander(std::string rCode)
{
	bool thereIsSomeData = provideDataSet();


	static const std::string rCmdDataName = "data", rCmdFiltered = "filteredData";


	if(thereIsSomeData)
	{
		rCode = ColumnEncoder::encodeAll(rCode);
		jaspRCPP_runScript((rCmdDataName + "<- .readFullDatasetToEnd();").c_str());
		jaspRCPP_runScript((rCmdFiltered + "<- .readFullFilteredDatasetToEnd();").c_str());
	}

	std::string rCodeResult =	jaspRCPP_evalRCodeCommander(rCode.c_str());

	if(thereIsSomeData)
	{
		rbridge_detachRCodeEnv(rCmdFiltered);
		rbridge_detachRCodeEnv(rCmdDataName);
		rCodeResult = ColumnEncoder::decodeAll(rCodeResult);
	}

	sendRCodeResult(rCodeResult, -1);

	_engineState = engineState::idle;
}


void Engine::sendRCodeResult(const std::string & rCodeResult, int rCodeRequestId)
{
	Json::Value rCodeResponse(Json::objectValue);

	std::string RError				= jaspRCPP_getLastErrorMsg();
	if(RError.size() > 0)
		rCodeResponse["rCodeError"]	= RError;

	rCodeResponse["typeRequest"]	= engineStateToString(engineState::rCode);
	rCodeResponse["rCodeResult"]	= rCodeResult;
	rCodeResponse["requestId"]		= rCodeRequestId;


	sendString(rCodeResponse.toStyledString());
}

void Engine::sendRCodeError(int rCodeRequestId)
{
	Log::log() << "R Code yielded error" << std::endl;

	Json::Value rCodeResponse		= Json::objectValue;
	std::string RError				= jaspRCPP_getLastErrorMsg();
	rCodeResponse["typeRequest"]	= engineStateToString(engineState::rCode);
	rCodeResponse["rCodeError"]		= RError.size() == 0 ? "R Code failed for unknown reason. Check that R function returns a string." : RError;
	rCodeResponse["requestId"]		= rCodeRequestId;

	sendString(rCodeResponse.toStyledString());
}

void Engine::receiveComputeColumnMessage(const Json::Value & jsonRequest)
{
	if(_engineState != engineState::idle)
		Log::log() << "Unexpected compute column message, current state is not idle (" << engineStateToString(_engineState) << ")";

	_engineState = engineState::computeColumn;

	std::string	computeColumnName =						 jsonRequest.get("columnName",  "").asString();
	std::string	computeColumnCode =						 jsonRequest.get("computeCode", "").asString();
	columnType	computeColumnType = columnTypeFromString(jsonRequest.get("columnType",  "").asString());

#ifdef JASP_COLUMN_ENCODE_ALL
	computeColumnName = ColumnEncoder::columnEncoder()->encode(computeColumnName);
#endif

	runComputeColumn(computeColumnName, computeColumnCode, computeColumnType);
}

void Engine::runComputeColumn(const std::string & computeColumnName, const std::string & computeColumnCode, columnType computeColumnType)
{
	Log::log() << "Engine::runComputeColumn()" << std::endl;

	static const std::map<columnType, std::string> setColumnFunction = {
		{columnType::scale,			".setColumnDataAsScale"			},
		{columnType::ordinal,		".setColumnDataAsOrdinal"		},
		{columnType::nominal,		".setColumnDataAsNominal"		},
		{columnType::nominalText,	".setColumnDataAsNominalText"	}};

	std::string computeColumnCodeComplete	= "local({;calcedVals <- {"+computeColumnCode +"};\n"  "return(toString(" + setColumnFunction.at(computeColumnType) + "('" + computeColumnName +"', calcedVals)));})";
	std::string computeColumnResultStr		= rbridge_evalRCodeWhiteListed(computeColumnCodeComplete);

	Json::Value computeColumnResponse		= Json::objectValue;
	computeColumnResponse["typeRequest"]	= engineStateToString(engineState::computeColumn);
	computeColumnResponse["result"]			= computeColumnResultStr;
	computeColumnResponse["error"]			= jaspRCPP_getLastErrorMsg();
	computeColumnResponse["columnName"]		= computeColumnName;

	sendString(computeColumnResponse.toStyledString());

	_engineState = engineState::idle;
}

void Engine::receiveModuleRequestMessage(const Json::Value & jsonRequest)
{
	_engineState					= engineStateFromString(jsonRequest.get("typeRequest", Json::nullValue).asString());

	std::string		moduleRequest	= jsonRequest["moduleRequest"].asString();
	std::string		moduleCode		= jsonRequest["moduleCode"].asString();
	std::string		moduleName		= jsonRequest["moduleName"].asString();

	std::string		result			= jaspRCPP_evalRCode(moduleCode.c_str());
	bool			succes			= result == "succes!"; //Defined in DynamicModule::succesResultString()

	Json::Value		jsonAnswer		= Json::objectValue;

	jsonAnswer["moduleRequest"]		= moduleRequest;
	jsonAnswer["moduleName"]		= moduleName;
	jsonAnswer["succes"]			= succes;
	jsonAnswer["error"]				= jaspRCPP_getLastErrorMsg();
	jsonAnswer["typeRequest"]		= engineStateToString(_engineState);

	sendString(jsonAnswer.toStyledString());

	_engineState = engineState::idle;
}

void Engine::receiveAnalysisMessage(const Json::Value & jsonRequest)
{
	if(_engineState != engineState::idle && _engineState != engineState::analysis)
		throw std::runtime_error("Unexpected analysis message, current state is not idle or analysis (" + engineStateToString(_engineState) + ")");

#ifdef PRINT_ENGINE_MESSAGES
	Log::log() << "Engine::receiveAnalysisMessage:\n" << jsonRequest.toStyledString() << std::endl;
#endif

	int analysisId		= jsonRequest.get("id", -1).asInt();
	performType perform	= performTypeFromString(jsonRequest.get("perform", "run").asString());

	if (analysisId == _analysisId && _analysisStatus == Status::running) // if the current running analysis has changed
		_analysisStatus = perform == performType::run ? Status::changed : Status::aborted;
	else
	{
		// the new analysis should be init or run (existing analyses will be aborted)
		_analysisId = analysisId;

		switch(perform)
		{
		case performType::run:			_analysisStatus = Status::toRun;		break;
		case performType::saveImg:		_analysisStatus = Status::saveImg;		break;
		case performType::editImg:		_analysisStatus = Status::editImg;		break;
		case performType::rewriteImgs:	_analysisStatus = Status::rewriteImgs;	break;
		default:						_analysisStatus = Status::error;		break;
		}

	}

#ifdef PRINT_ENGINE_MESSAGES
	Log::log() << "msg type was '" << engineAnalysisStatusToString(_analysisStatus) << "'" << std::endl;
#endif

	if(	_analysisStatus == Status::toRun		||
		_analysisStatus == Status::changed		||
		_analysisStatus == Status::saveImg		||
		_analysisStatus == Status::editImg		||
		_analysisStatus == Status::rewriteImgs	  )
	{
		_analysisName			= jsonRequest.get("name",				Json::nullValue).asString();
		_analysisTitle			= jsonRequest.get("title",				Json::nullValue).asString();
		_analysisDataKey		= jsonRequest.get("dataKey",			Json::nullValue).toStyledString();
		_analysisResultsMeta	= jsonRequest.get("resultsMeta",		Json::nullValue).toStyledString();
		_analysisStateKey		= jsonRequest.get("stateKey",			Json::nullValue).toStyledString();
		_analysisRevision		= jsonRequest.get("revision",			-1).asInt();
		_imageOptions			= jsonRequest.get("image",				Json::nullValue);
		_analysisRFile			= jsonRequest.get("rfile",				"").asString();
		_dynamicModuleCall		= jsonRequest.get("dynamicModuleCall",	"").asString();
		_engineState			= engineState::analysis;

		Json::Value optionsEnc	= jsonRequest.get("options",			Json::nullValue);
		
		_extraEncodings->setCurrentNamesFromOptionsMeta(optionsEnc);
		
#ifdef JASP_COLUMN_ENCODE_ALL
		encodeColumnNamesinOptions(optionsEnc);
#endif
		_analysisOptions		= optionsEnc.toStyledString();
	}
}

void Engine::encodeColumnNamesinOptions(Json::Value & options)
{
	_encodeColumnNamesinOptions(options, options[".meta"]);
}

void Engine::_encodeColumnNamesinOptions(Json::Value & options, Json::Value & meta)
{
#ifdef JASP_COLUMN_ENCODE_ALL
	if(meta.isNull())
		return;
	
	bool	encodePlease	= meta.isObject() && meta.get("shouldEncode",	false).asBool(),
			isRCode			= meta.isObject() && meta.get("rCode",			false).asBool();

	switch(options.type())
	{
	case Json::arrayValue:
		if(encodePlease)
			ColumnEncoder::columnEncoder()->encodeJson(options, false); //If we already think we have columnNames just change it all
		
		else if(meta.type() == Json::arrayValue)
			for(size_t i=0; i<options.size() && i < meta.size(); i++)
				_encodeColumnNamesinOptions(options[i], meta[i]);
		
		else if(isRCode)
			for(size_t i=0; i<options.size(); i++)
				if(options[i].isString())
					options[i] = ColumnEncoder::columnEncoder()->encodeRScript(options[i].asString());
	
		return;

	case Json::objectValue:
		for(const std::string & memberName : options.getMemberNames())
			if(memberName != ".meta" && meta.isMember(memberName))
				_encodeColumnNamesinOptions(options[memberName], meta[memberName]);
		
			else if(isRCode && options[memberName].isString())
				options[memberName] = ColumnEncoder::columnEncoder()->encodeRScript(options[memberName].asString());
		
			else if(encodePlease)
				ColumnEncoder::columnEncoder()->encodeJson(options, false); //If we already think we have columnNames just change it all I guess?
		
		return;

	case Json::stringValue:
			
			if(isRCode)				options = ColumnEncoder::columnEncoder()->encodeRScript(options.asString());
			else if(encodePlease)	options = ColumnEncoder::columnEncoder()->encodeAll(options.asString());
			
		return;

	default:
		return;
	}
#endif
}

void Engine::sendString(std::string message)
{
	Utils::convertEscapedUnicodeToUTF8(message);

	Json::Value msgJson;

	if(Json::Reader().parse(message, msgJson)) //If everything is converted to jaspResults maybe we can do this there?
	{
#ifdef JASP_COLUMN_ENCODE_ALL
		ColumnEncoder::columnEncoder()->decodeJson(msgJson); // decode all columnnames as far as you can
#endif
		_channel->send(msgJson.toStyledString());
	}
	else
		_channel->send(message);
}


void Engine::runAnalysis()
{
	Log::log() << "Engine::runAnalysis() " << _analysisTitle << " (" << _analysisId << ") revision: " << _analysisRevision << std::endl;

	switch(_analysisStatus)
	{
	case Status::saveImg:		 saveImage();						return;
	case Status::editImg:		 editImage();						return;
	case Status::rewriteImgs:	 rewriteImages();					return;

	case Status::empty:
	case Status::aborted:
		_analysisStatus	= Status::empty;
		_engineState	= engineState::idle;
		Log::log() << "Engine::state <= idle because it does not need to be run now (empty || aborted)" << std::endl;
		return;

	default:	break;
	}

	Log::log() << "Analysis will be run now." << std::endl;

	_analysisResultsString = rbridge_runModuleCall(_analysisName, _analysisTitle, _dynamicModuleCall, _analysisDataKey, _analysisOptions, _analysisStateKey, _ppi, _analysisId, _analysisRevision, _imageBackground, _developerMode);

	switch(_analysisStatus)
	{
	case Status::aborted:
	case Status::error:
	case Status::exception:
		return;

	case Status::changed:
		// analysis was changed, and the analysis killed itself through jaspResults::checkForAnalysisChanged()
		//It needs to be re-run and the tempfiles can be cleared.
		_analysisStatus = Status::toRun;
		TempFiles::deleteList(TempFiles::retrieveList(_analysisId));
		return;

	default:
		Json::Reader().parse(_analysisResultsString, _analysisResults, false);

		_engineState	= engineState::idle;
		_analysisStatus	= Status::empty;

		removeNonKeepFiles(_analysisResults.isObject() ? _analysisResults.get("keep", Json::nullValue) : Json::nullValue);
		return;
	}
}

void Engine::saveImage()
{
	int			height	= _imageOptions.get("height",	Json::nullValue).asInt(),
				width	= _imageOptions.get("width",	Json::nullValue).asInt();
	std::string data	= _imageOptions.get("data",		Json::nullValue).asString(),
				type	= _imageOptions.get("type",		Json::nullValue).asString(),
				result	= jaspRCPP_saveImage(data.c_str(), type.c_str(), height, width, _ppi, _imageBackground.c_str());

	Json::Reader().parse(result, _analysisResults, false);

	_analysisStatus								= Status::complete;
	_analysisResults["results"]["inputOptions"]	= _imageOptions;

	sendAnalysisResults();

	_analysisStatus								= Status::empty;
	_engineState								= engineState::idle;
}

void Engine::editImage()
{
	std::string optionsJson	= _imageOptions.toStyledString(),
				result		= jaspRCPP_editImage(optionsJson.c_str(), _ppi, _imageBackground.c_str());

	Json::Reader().parse(result, _analysisResults, false);

	if(_analysisResults.isMember("results"))
		_analysisResults["results"]["request"] = _imageOptions.get("request", -1);

	_analysisStatus			= Status::complete;

	sendAnalysisResults();

	_analysisStatus			= Status::empty;
	_engineState			= engineState::idle;
}

void Engine::rewriteImages()
{
	jaspRCPP_rewriteImages(_ppi, _imageBackground.c_str());

	_analysisStatus				= Status::complete;
	_analysisResults			= Json::Value();
	_analysisResults["status"]	= analysisResultStatusToString(analysisResultStatus::imagesRewritten);

	sendAnalysisResults();

	_analysisStatus				= Status::empty;
	_engineState				= engineState::idle;
}


analysisResultStatus Engine::getStatusToAnalysisStatus()
{
	switch (_analysisStatus)
	{
	case Status::running:
	case Status::changed:	return analysisResultStatus::running;
	case Status::complete:	return analysisResultStatus::complete;
	default:				return analysisResultStatus::fatalError;
	}
}

void Engine::sendAnalysisResults()
{
	Json::Value response			= Json::Value(Json::objectValue);

	response["typeRequest"]			= engineStateToString(engineState::analysis);
	response["id"]					= _analysisId;
	response["name"]				= _analysisName;
	response["revision"]			= _analysisRevision;
	response["progress"]			= Json::nullValue;
	
	bool					sensibleResultsStatus	= _analysisResults.isObject() && _analysisResults.get("status", Json::nullValue) != Json::nullValue;
	analysisResultStatus	resultStatus			= !sensibleResultsStatus ? getStatusToAnalysisStatus() : analysisResultStatusFromString(_analysisResults["status"].asString());

	response["results"] = _analysisResults.get("results", _analysisResults);
	response["status"]  = analysisResultStatusToString(resultStatus);

	sendString(response.toStyledString());
}

void Engine::removeNonKeepFiles(const Json::Value & filesToKeepValue)
{
	std::vector<std::string> filesToKeep;

	if (filesToKeepValue.isArray())
	{
		for (size_t i = 0; i < filesToKeepValue.size(); i++)
		{
			Json::Value fileToKeepValue = filesToKeepValue.get(i, Json::nullValue);
			if ( ! fileToKeepValue.isString())
				continue;

			filesToKeep.push_back(fileToKeepValue.asString());
		}
	}
	else if (filesToKeepValue.isString())
	{
		filesToKeep.push_back(filesToKeepValue.asString());
	}

	std::vector<std::string> tempFilesFromLastTime = TempFiles::retrieveList(_analysisId);

	Utils::remove(tempFilesFromLastTime, filesToKeep);

	TempFiles::deleteList(tempFilesFromLastTime);
}

DataSet * Engine::provideDataSet()
{
	JASPTIMER_RESUME(Engine::provideDataSet());
	DataSet * dataset = SharedMemory::retrieveDataSet(_parentPID);
	JASPTIMER_STOP(Engine::provideDataSet());

	return dataset;
}

void Engine::provideStateFileName(std::string & root, std::string & relativePath)
{
	return TempFiles::createSpecific("state", _analysisId, root, relativePath);
}

void Engine::provideJaspResultsFileName(std::string & root, std::string & relativePath)
{
	return TempFiles::createSpecific("jaspResults.json", _analysisId, root, relativePath);
}

void Engine::provideSpecificFileName(const std::string & specificName, std::string & root, std::string & relativePath)
{
	return TempFiles::createSpecific(specificName, _analysisId, root, relativePath);
}

void Engine::provideTempFileName(const std::string & extension, std::string & root, std::string & relativePath)
{
	TempFiles::create(extension, _analysisId, root, relativePath);
}

bool Engine::isColumnNameOk(std::string columnName)
{
	if(columnName == "" || !provideDataSet())
		return false;

	try
	{
		provideDataSet()->columns().findIndexByName(columnName);
		return true;
	}
	catch(columnNotFound &)
	{
		return false;
	}
}

bool Engine::setColumnDataAsNominalOrOrdinal(bool isOrdinal, const std::string & columnName, std::vector<int> & data, const std::map<int, std::string> & levels)
{
	std::map<int, int> uniqueInts;

	for(auto keyval : levels)
	{
		size_t convertedChars;

		try {
			int asInt = std::stoi(keyval.second, &convertedChars);

			if(convertedChars == keyval.second.size()) //It was a number!
				uniqueInts[keyval.first] = asInt;
		}
		catch(std::invalid_argument & e) {}
	}

	if(uniqueInts.size() == levels.size()) //everything was an int!
	{
		for(auto & dat : data)
			if(dat != INT_MIN)
				dat = uniqueInts[dat];

		if(isOrdinal)	return	provideDataSet()->columns()[columnName].overwriteDataWithOrdinal(data);
		else			return	provideDataSet()->columns()[columnName].overwriteDataWithNominal(data);
	}
	else
	{
		if(isOrdinal)	return	provideDataSet()->columns()[columnName].overwriteDataWithOrdinal(data, levels);
		else			return	provideDataSet()->columns()[columnName].overwriteDataWithNominal(data, levels);
	}
}

void Engine::stopEngine()
{
	Log::log() << "Engine::stopEngine() received, closing engine." << std::endl;

	switch(_engineState)
	{
	default:							/* everything not mentioned is fine */	break;
	case engineState::analysis:			_analysisStatus = Status::aborted;		break;
	case engineState::filter:
	case engineState::computeColumn:	throw std::runtime_error("Unexpected data synch during " + engineStateToString(_engineState) + " somehow, you should not expect to see this exception ever.");
	};

	_engineState = engineState::stopped;

	freeRBridgeColumns();
	SharedMemory::unloadDataSet();
	sendEngineStopped();
}

void Engine::sendEngineStopped()
{
	Json::Value rCodeResponse		= Json::objectValue;
	rCodeResponse["typeRequest"]	= engineStateToString(_engineState);
	sendString(rCodeResponse.toStyledString());
}

void Engine::pauseEngine()
{
	Log::log() << "Engine paused" << std::endl;

	switch(_engineState)
	{
	default:							/* everything not mentioned is fine */	break;
	case engineState::analysis:			_analysisStatus = Status::aborted;		break;
	case engineState::filter:
	case engineState::computeColumn:	throw std::runtime_error("Unexpected data synch during " + engineStateToString(_engineState) + " somehow, you should not expect to see this exception ever.");
	};

	_engineState = engineState::paused;

	freeRBridgeColumns();
	SharedMemory::unloadDataSet();
	sendEnginePaused();
}

void Engine::sendEnginePaused()
{
	Json::Value rCodeResponse		= Json::objectValue;
	rCodeResponse["typeRequest"]	= engineStateToString(engineState::paused);

	sendString(rCodeResponse.toStyledString());
}

void Engine::resumeEngine(const Json::Value & jsonRequest)
{
	Log::log() << "Engine resuming, absorbing settings and rescanning columnNames for en/decoding" << std::endl;
	//Any changes to the data that engine needs to know about are accompanied by pause + resume I think.
	ColumnEncoder::columnEncoder()->setCurrentColumnNames(provideDataSet() == nullptr ? std::vector<std::string>({}) : provideDataSet()->getColumnNames());

	absorbSettings(jsonRequest);

	_engineState = engineState::idle;
	sendEngineResumed();
}

void Engine::sendEngineResumed()
{

	Json::Value rCodeResponse		= Json::objectValue;
	rCodeResponse["typeRequest"]	= engineStateToString(engineState::resuming);

	sendString(rCodeResponse.toStyledString());	
}

void Engine::receiveLogCfg(const Json::Value & jsonRequest)
{
	Log::log() << "Log Config received" << std::endl;

	Log::parseLogCfgMsg(jsonRequest);

	Json::Value logCfgResponse		= Json::objectValue;
	logCfgResponse["typeRequest"]	= engineStateToString(engineState::logCfg);

	sendString(logCfgResponse.toStyledString());

	_engineState = engineState::idle;
}

void Engine::absorbSettings(const Json::Value & jsonRequest)
{
	_ppi				= jsonRequest.get("ppi",				_ppi			).asInt();
	_developerMode		= jsonRequest.get("developerMode",		_developerMode	).asBool();
	_imageBackground	= jsonRequest.get("imageBackground",	_imageBackground).asString();
	_langR				= jsonRequest.get("languageCode",		_langR			).asString();

	rbridge_setLANG(_langR);
}


void Engine::receiveSettings(const Json::Value & jsonRequest)
{
	Log::log() << "Settings received" << std::endl;

	absorbSettings(jsonRequest);

	Json::Value response	= Json::objectValue;
	response["typeRequest"]	= engineStateToString(engineState::settings);

	sendString(response.toStyledString());

	_engineState = engineState::idle;
}
