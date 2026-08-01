/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#include "security/rsa.hpp"
#include "server/server.hpp"

class Logger;
class BotManager;

class FailedToInitializeCanary : public std::exception {
private:
	std::string message;

public:
	// Constructor accepts a specific message
	explicit FailedToInitializeCanary(const std::string &msg) :
		message("Canary load couldn't be completed. " + msg) { }

	// Override the what() method from std::exception
	const char* what() const noexcept override {
		return message.c_str();
	}
};

class CanaryServer {
public:
	explicit CanaryServer(
		Logger &logger,
		RSAManager &rsa,
		ServiceManager &serviceManager
	);
	~CanaryServer();

	int run();
	int generateLuaApiDocsOnly();

private:
	enum class LoaderStatus : uint8_t {
		LOADING,
		LOADED,
		FAILED
	};

	Logger &logger;
	RSAManager &rsa;
	ServiceManager &serviceManager;
	std::unique_ptr<BotManager> botManager;

	LoaderStatus loaderStatus = LoaderStatus::LOADING;
	std::mutex loaderMutex;
	std::condition_variable loaderCV;

	void logInfos();
	static void toggleForceCloseButton();
	static void badAllocationHandler();
	void shutdown();

	static std::string getCompiler();
	static std::string getPlatform();

	void loadConfigLua();
	bool generateLuaApiDocs(bool force = false) const;
	void validateDatapack();
	void initializeDatabase();
	void loadModules();
	void setWorldType();
	void loadMaps() const;
	void setupHousesRent();
	void modulesLoadHelper(bool loaded, std::string_view identifier);
};
