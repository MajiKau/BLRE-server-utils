// include sdk headers to communicate with UE3
// WARNING: this header file can currently only be included once!
//   the SDK currently throws alot of warnings which can be ignored
#pragma warning(disable:4244)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <vector>
#include <SdkHeaders.h>

#include <fstream>

// for access to event manager
#include <Proxy/Events.h>
// for access to client/server
#include <Proxy/Network.h>
// for module api
#include <Proxy/Modules.h>
// for logging in main file
#include <Proxy/Logger.h>
using namespace BLRevive;

#define NUM_BOT_NAMES 19

struct ServerProperties {
	// can't set this early enough for it to take effect
	// float MaxIntermissionIdle;
	std::string RandomBotNames[NUM_BOT_NAMES];
	float GameRespawnTime;
	float GameForceRespawnTime;
	float GameSpectatorSwitchDelayTime;
	int NumEnemyVotesRequiredForKick;
	int NumFriendlyVotesRequiredForKick;
	int VoteKickBanSeconds;
	float MaxIdleTime;
	int MinRequiredPlayersToStart;
	float PlayerSearchTime;
	int TimeLimit;
	int GoalScore;
};

struct ServerHacks {
	bool disableOnMatchIdle;
};

struct ServerMutators {
	bool DisableDepots;
	bool DisableHRV;
	bool DisableHeadShots;
	bool StockLoadout;
	bool DisablePrimaries;
	bool DisableSecondaries;
	bool DisableGear;
	bool DisableTacticalGear;
	bool DisableHealthRegen;
	bool DisableElementalAmmo;
	bool HeadshotsOnly;
};

struct ServerConfig {
	ServerProperties properties;
	ServerHacks hacks;
	ServerMutators mutators;

	char RandomBotNamesFString[NUM_BOT_NAMES * sizeof(FString)];
	char RandomBotNamesFStringBackup[NUM_BOT_NAMES * sizeof(FString)];
};

static ServerConfig serverConfig;
HANDLE serverInfoMutex;
json serverInfo;
static void applyConfigRuntime(AFoxGame *game, const ServerConfig &config);
static void applyParametersToGameDefault(const ServerConfig &config);
static void applyParametersToGameObject(AFoxGame *game, const ServerConfig &config);
static void backupAndSetBotnamesToGameDefault(ServerConfig &config);
static void restoreBotnamesToGameDefault(const ServerConfig &config);
static void applyOneshotHacks(const ServerConfig &config);
static void applyRecurringHacks(AFoxGame *game, const ServerConfig &config);
static void getRequestHandler(const httplib::Request &req, httplib::Response &res);
static void writeServerInfo();
static void updateServerInfo(AFoxGame* game);
static void threadLoop();

static ServerConfig serverConfigFromFile();

static void logError(std::string message) {
	LError("server-utils: " + message);
	LFlush;
}

static void logDebug(std::string message) {
	LDebug("server-utils: " + message);
	LFlush;
}

static void logWarn(std::string message) {
	LWarn("server-utils: " + message);
	LFlush;
}

/// <summary>
/// Thread thats specific to the module (function must exist and export demangled!)
/// </summary>
extern "C" __declspec(dllexport) void ModuleThread()
{
	if (!Utils::IsServer()) {
		return;
	}

	serverInfoMutex = CreateMutex(NULL, FALSE, NULL);

	serverConfig = serverConfigFromFile();

	std::shared_ptr<Event::Manager> eventManager = Event::Manager::GetInstance();
#if 1
	eventManager->RegisterHandler({
		64192, //Events::ID("*", "IsConfigFiltered"),
		[=](Event::Info info) {
			backupAndSetBotnamesToGameDefault(serverConfig);
			applyParametersToGameDefault(serverConfig);
			applyOneshotHacks(serverConfig);
			return true;
		}});
	logDebug("registered oneshot handler for event * IsConfigFiltered");
#endif
#if 1
	auto updateGameSettingsRestoreBotnamesCallback = [=](Event::Info info) {
		AFoxGame* game = (AFoxGame*)info.Object;
		restoreBotnamesToGameDefault(serverConfig);
		return true;
	};

	eventManager->RegisterHandler({19089, updateGameSettingsRestoreBotnamesCallback });
	eventManager->RegisterHandler({52580, updateGameSettingsRestoreBotnamesCallback });
	logDebug("registered oneshot handler for event * UpdateGameSettings");
#endif
#if 1
	auto updateGameSettingsCallback = [=](Event::Info info) {
		AFoxGame* game = (AFoxGame*)info.Object;
		applyRecurringHacks(game, serverConfig);
		applyParametersToGameObject(game, serverConfig);
		updateServerInfo(game);
		return false;
	};

	eventManager->RegisterHandler({ 19089, updateGameSettingsCallback });
	eventManager->RegisterHandler({ 52580, updateGameSettingsCallback });
	logDebug("registered handler for event * UpdateGameSettings");
#endif
#if 0
	eventManager->RegisterHandler({
		Events::ID("*", "*"),
		[=](Events::Info info) {
		},
		true});
	logDebug("registered handler for event * *");
#endif

	while(true){
		Sleep(5000);
		threadLoop();
	}
}

/// <summary>
/// Module initializer (function must exist and export demangled!)
/// </summary>
/// <param name="data"></param>
extern "C" __declspec(dllexport) void InitializeModule(Module::InitData *data)
{
    // check param validity
    if (!data || !data->EventManager || !data->Logger) {
        LError("module initializer param was null!"); LFlush;
        return;
    }

    // initialize logger (to enable logging to the same file)
    Logger::Link(data->Logger);

    // initialize event manager
    // an instance of the manager can be retrieved with Event::Manager::Instance() afterwards
    Event::Manager::Link(data->EventManager);

	// handle get requests
	data->Server->AddConnectionHandler(Network::RequestType::GET, "/server_info", [&](const httplib::Request &req, httplib::Response &res)
								 { getRequestHandler(req, res); });

    // initialize your module
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

static void threadLoop(){
	writeServerInfo();
}

static std::string getConfigPath() {
	static char *pathStr = nullptr;
	if (pathStr == nullptr) {
		std::string path = Utils::FS::BlreviveConfigPath();
		pathStr = new char[path.length() + 1];
		strcpy(pathStr, path.c_str());
		return path;
	}
	else {
		return std::string(pathStr);
	}
}

static std::string getOutputPath(){
	std::string outputPath = getConfigPath() + "server_utils/";

	struct stat info;
	if (stat(outputPath.c_str(), &info) == 0) {
		// path exists
		if (info.st_mode & S_IFDIR) {
			// path exists and is a dir
			return outputPath;
		}
		else {
			logError(fmt::format("{0} exists but it is not a directory", outputPath));
			return "";
		}
		}
		else {
		// path do not exist
		if (CreateDirectory(outputPath.c_str(), nullptr)) {
			return outputPath;
		}
		else {
			logError(fmt::format("cannot create directory {0}", outputPath));
			return "";
		}
	}
}

static void writeConfig(json &toWrite, std::string path){
	std::ofstream output(path);
	if(!output.is_open()){
		logError(fmt::format("failed writing config to {0}", path));
		return;
	}
	output << toWrite.dump(4) << std::endl;
	output.close();
	return;
}

static json defaultConfigJson(){
	json defaultConfig;
	// defaultConfig["properties"]["MaxIntermissionIdle"] = 3600.0;
	for(int i = 0;i < NUM_BOT_NAMES; i++){
		defaultConfig["properties"]["RandomBotNames"][i] = fmt::format("blrevive bot MK{0}", i);
	}
	defaultConfig["properties"]["GameRespawnTime"] = 10.0;
	defaultConfig["properties"]["GameForceRespawnTime"] = 30.0;
	defaultConfig["properties"]["GameSpectatorSwitchDelayTime"] = 120.0;
	defaultConfig["properties"]["NumEnemyVotesRequiredForKick"] = 4;
	defaultConfig["properties"]["NumFriendlyVotesRequiredForKick"] = 2;
	defaultConfig["properties"]["VoteKickBanSeconds"] = 1200;
	defaultConfig["properties"]["MaxIdleTime"] = 180.0;
	defaultConfig["properties"]["MinRequiredPlayersToStart"] = 1;
	defaultConfig["properties"]["PlayerSearchTime"] = 30.0;
	defaultConfig["properties"]["TimeLimit"] = 10;
	defaultConfig["properties"]["GoalScore"] = 3000;

	defaultConfig["hacks"]["disableOnMatchIdle"] = 1;

	defaultConfig["mutators"]["DisableDepots"] = 0;
	defaultConfig["mutators"]["DisableHRV"] = 0;
	defaultConfig["mutators"]["DisableHeadShots"] = 0;
	defaultConfig["mutators"]["StockLoadout"] = 0;
	defaultConfig["mutators"]["DisablePrimaries"] = 0;
	defaultConfig["mutators"]["DisableSecondaries"] = 0;
	defaultConfig["mutators"]["DisableGear"] = 0;
	defaultConfig["mutators"]["DisableTacticalGear"] = 0;
	defaultConfig["mutators"]["DisableHealthRegen"] = 0;
	defaultConfig["mutators"]["DisableElementalAmmo"] = 0;
	defaultConfig["mutators"]["HeadshotsOnly"] = 0;

	return defaultConfig;
}

static json getJsonValue(json &input, json &defaultOverlay, std::string category, std::string param){
	json val;
	try{
		val = input[category][param];
		if(val.is_null()){
			logWarn(fmt::format("{0}/{1} not found in config, using default value", category, param));
			val = defaultOverlay[category][param];
		}
	}catch(json::exception e){
		throw(e);
	}
	return val;
}

static ServerConfig serverConfigFromJson(json input){
	ServerConfig config;
	json defaultConfig = defaultConfigJson();
	json val;
	try{
		// config.MaxIntermissionIdle = input["properties"]["MaxIntermissionIdle"];
		json botNameArray = getJsonValue(input, defaultConfig, "properties", "RandomBotNames");
		for(int i = 0; i < NUM_BOT_NAMES; i++){
			json botName = botNameArray[i];
			if(botName.is_null()){
				logWarn(fmt::format("the #{0} bot name is not provided, using default value", i));
				botName = defaultConfig["properties"]["RandomBotNames"][i];
			}
			config.properties.RandomBotNames[i] = botName;
			FString newName(config.properties.RandomBotNames[i].c_str());
			memcpy(&(config.RandomBotNamesFString[i * sizeof(FString)]), &newName, sizeof(FString));
		}
		config.properties.GameRespawnTime = getJsonValue(input, defaultConfig, "properties", "GameRespawnTime");
		config.properties.GameForceRespawnTime = getJsonValue(input, defaultConfig, "properties", "GameForceRespawnTime");
		config.properties.NumEnemyVotesRequiredForKick = getJsonValue(input, defaultConfig, "properties", "NumEnemyVotesRequiredForKick");
		config.properties.NumFriendlyVotesRequiredForKick = getJsonValue(input, defaultConfig, "properties", "NumFriendlyVotesRequiredForKick");
		config.properties.VoteKickBanSeconds = getJsonValue(input, defaultConfig, "properties", "VoteKickBanSeconds");
		config.properties.MaxIdleTime = getJsonValue(input, defaultConfig, "properties", "MaxIdleTime");
		config.properties.MinRequiredPlayersToStart = getJsonValue(input, defaultConfig, "properties", "MinRequiredPlayersToStart");
		config.properties.PlayerSearchTime = getJsonValue(input, defaultConfig, "properties", "PlayerSearchTime");
		config.properties.TimeLimit = getJsonValue(input, defaultConfig, "properties", "TimeLimit");
		config.properties.GoalScore = getJsonValue(input, defaultConfig, "properties", "GoalScore");

		config.hacks.disableOnMatchIdle = (int)getJsonValue(input, defaultConfig, "hacks", "disableOnMatchIdle");

		config.mutators.DisableDepots = (int)getJsonValue(input, defaultConfig, "mutators", "DisableDepots");
		config.mutators.DisableHRV = (int)getJsonValue(input, defaultConfig, "mutators", "DisableHRV");
		config.mutators.DisableHeadShots = (int)getJsonValue(input, defaultConfig, "mutators", "DisableHeadShots");
		config.mutators.StockLoadout = (int)getJsonValue(input, defaultConfig, "mutators", "StockLoadout");
		config.mutators.DisablePrimaries = (int)getJsonValue(input, defaultConfig, "mutators", "DisablePrimaries");
		config.mutators.DisableSecondaries = (int)getJsonValue(input, defaultConfig, "mutators", "DisableSecondaries");
		config.mutators.DisableGear = (int)getJsonValue(input, defaultConfig, "mutators", "DisableGear");
		config.mutators.DisableTacticalGear = (int)getJsonValue(input, defaultConfig, "mutators", "DisableTacticalGear");
		config.mutators.DisableHealthRegen = (int)getJsonValue(input, defaultConfig, "mutators", "DisableHealthRegen");
		config.mutators.DisableElementalAmmo = (int)getJsonValue(input, defaultConfig, "mutators", "DisableElementalAmmo");
		config.mutators.HeadshotsOnly = (int)getJsonValue(input, defaultConfig, "mutators", "HeadshotsOnly");

	}catch(json::exception e){
		throw(e);
	}
	return config;
}

static ServerConfig serverConfigFromFile(){
	std::string outputPath = getOutputPath();
	if(outputPath.length() == 0){
		logError(fmt::format("cannot load config from {0}", outputPath));
		return serverConfigFromJson(defaultConfigJson());
	}

	std::string serverConfigPath = fmt::format("{0}{1}", outputPath, "server_config.json");
	std::ifstream input(serverConfigPath);
	if(!input.is_open()){
		logDebug(fmt::format("{0} does not exist, writing default config", serverConfigPath));
		json config = defaultConfigJson();
		writeConfig(config, serverConfigPath);
		return serverConfigFromJson(config);
	}

	try{
		json inputJson = json::parse(input);
		input.close();
		return serverConfigFromJson(inputJson);
	}catch(json::exception e){
		logError(fmt::format("failed parsing {0}, using default config", serverConfigPath));
		logError(e.what());
		input.close();
		return serverConfigFromJson(defaultConfigJson());
	}
}

static void applyParametersToGameDefault(const ServerConfig &config){
	UFoxIntermission *intermission = UObject::GetInstanceOf<UFoxIntermission>(true);
	intermission->PlayerSearchTime = config.properties.PlayerSearchTime;
}

static void applyParametersToGameObject(AFoxGame *game, const ServerConfig &config){
	// game->MaxIntermissionIdle = config.properties.MaxIntermissionIdle;
	game->GameRespawnTime = config.properties.GameRespawnTime;
	game->GameForceRespawnTime = config.properties.GameForceRespawnTime;
	game->NumEnemyVotesRequiredForKick = config.properties.NumEnemyVotesRequiredForKick;
	game->NumFriendlyVotesRequiredForKick = config.properties.NumFriendlyVotesRequiredForKick;
	game->VoteKickBanSeconds = config.properties.VoteKickBanSeconds;
	game->MaxIdleTime = config.properties.MaxIdleTime;
	game->GoalScore = config.properties.GoalScore;
	game->FGRI->GoalScore = config.properties.GoalScore;
	if (game->FGRI->GameStatus == 1){
		UFoxIntermission *intermission = (UFoxIntermission *)game->FGRI->GameFlow;
		intermission->MinRequiredPlayersToStart = config.properties.MinRequiredPlayersToStart;
		game->TimeLimit = config.properties.TimeLimit;
		game->FGRI->TimeLimit = game->TimeLimit;
		game->FGRI->RemainingMinute = game->TimeLimit;
		game->FGRI->RemainingTime = game->TimeLimit * 60;
	}

	// More mutators in Engine_classes.h line 2382
	game->FGRI->SetMutatorEnabled(3, config.mutators.DisableDepots);
	game->FGRI->SetMutatorEnabled(4, config.mutators.DisableHRV);
	game->FGRI->SetMutatorEnabled(6, config.mutators.DisableHeadShots);
	game->FGRI->SetMutatorEnabled(7, config.mutators.StockLoadout);
	game->FGRI->SetMutatorEnabled(8, config.mutators.DisablePrimaries);
	game->FGRI->SetMutatorEnabled(9, config.mutators.DisableSecondaries);
	game->FGRI->SetMutatorEnabled(10, config.mutators.DisableGear);
	game->FGRI->SetMutatorEnabled(11, config.mutators.DisableTacticalGear);
	game->FGRI->SetMutatorEnabled(12, config.mutators.DisableHealthRegen);
	game->FGRI->SetMutatorEnabled(13, config.mutators.DisableElementalAmmo);
	game->FGRI->SetMutatorEnabled(14, config.mutators.HeadshotsOnly);
	game->FGRI->ApplyMutators(); // Not sure this actually does anything, but it does no harm

}

static std::string unrealStringToString(FString &value){
	if(value.Data == NULL){
		return std::string("");
	}
	const char *cstr = value.ToChar();
	std::string ret = std::string(cstr);
	free((void *)cstr);
	return ret;
}

static void getRequestHandler(const httplib::Request &req, httplib::Response &res){
	WaitForSingleObject(serverInfoMutex, INFINITE);
	std::string outputString = serverInfo.dump();
	ReleaseMutex(serverInfoMutex);
	// rewrite the 401 from access control, this should be available to everyone
	// this might change in proxy, it does look like HandlerResponse::Unhandled and HandlerResponse::Handled were flipped
	res.status = 200;
	res.set_content(outputString, "application/json");
}

static void writeServerInfo(){
	std::string outputPath = getOutputPath();
	if (outputPath.length() == 0) {
		logError("cannot write server_info.json, destination is inaccessible");
		return;
	}
	std::string path = fmt::format("{0}{1}", outputPath, "server_info.json");

	bool mutexReleased = true;
	try
	{
		std::ofstream output(path);
		if (!output.is_open()) {
			logError(fmt::format("failed writing server info to {0}", path));
			return;
		}

		WaitForSingleObject(serverInfoMutex, INFINITE);
		std::string outputString = serverInfo.dump(4);
		ReleaseMutex(serverInfoMutex);
		output << outputString << std::endl;
		output.close();
	}
	catch (std::exception e)
	{
		logError(fmt::format("failed saving {0}", path));
		logError(e.what());
	};
}

static void updateServerInfo(AFoxGame* game)
{
	// both FString manipulation and invoking game ticks tends to get crashy, but it seems to work here
	// can't find an alternative yet however
	FString mapNameFString = game->WorldInfo->GetMapName(true);
	std::string mapName = unrealStringToString(mapNameFString);
	std::string serverName = unrealStringToString(game->FGRI->ServerName);
	std::string playlist = game->FGRI->playlistName.GetName();

	TArray<AFoxTeamInfo*> teams = game->Teams;
	TArray<APlayerReplicationInfo*> players = game->GameReplicationInfo->PRIArray;

	WaitForSingleObject(serverInfoMutex, INFINITE);
	int botCount = 0;
	int playerCount = 0;
	serverInfo["TeamList"] = json::array();
	// there seems to always be a dummy team
	for (int i = 0; i < (game->NumTeams - 1) && i < (teams.Count - 1); i++){
		AFoxTeamInfo* team = teams.at(i);
		serverInfo["TeamList"][i]["PlayerList"] = json::array();
		serverInfo["TeamList"][i]["BotList"] = json::array();
		int teamBotCount = 0;
		int teamPlayerCount = 0;
		for (int j = 0; j < players.Count; j++)
		{
			AFoxPRI* player = (AFoxPRI *)players.at(j);
			if((void *)team != (void *)player->Team){
				continue;
			}
			std::string targetList;
			int targetIndex;
			if (player->bBot) {
				targetIndex = teamBotCount;
				teamBotCount++;
				targetList = std::string("BotList");
			}else{
				targetIndex = teamPlayerCount;
				teamPlayerCount++;
				targetList = std::string("PlayerList");
			}
			serverInfo["TeamList"][i][targetList][targetIndex]["Name"] = unrealStringToString(player->PlayerName);
			serverInfo["TeamList"][i][targetList][targetIndex]["Kills"] = player->TotalKills;
			serverInfo["TeamList"][i][targetList][targetIndex]["Deaths"] = player->TotalDeaths;
			serverInfo["TeamList"][i][targetList][targetIndex]["Score"] = player->TotalEarnedXP;
		}
		serverInfo["TeamList"][i]["PlayerCount"] = teamPlayerCount;
		serverInfo["TeamList"][i]["BotCount"] = teamBotCount;
		serverInfo["TeamList"][i]["TeamScore"] = (int)team->Score;
		botCount += teamBotCount;
		playerCount += teamPlayerCount;
	}

	serverInfo["PlayerCount"] = playerCount;
	serverInfo["BotCount"] = botCount;
	serverInfo["Map"] = mapName;
	serverInfo["ServerName"] = serverName;
	serverInfo["GameMode"] = playlist;
	serverInfo["GoalScore"] = game->GoalScore;
	serverInfo["TimeLimit"] = game->FGRI->TimeLimit * 60;
	serverInfo["RemainingTime"] = game->FGRI->RemainingTime;
	serverInfo["MaxPlayers"] = game->FGRI->MaxPlayers;

	ReleaseMutex(serverInfoMutex);
}

static void backupAndSetBotnamesToGameDefault(ServerConfig &config){
	AFoxGame *game = UObject::GetInstanceOf<AFoxGame>(true);
	for(int i = 0;i < NUM_BOT_NAMES;i++){
		memcpy(&(config.RandomBotNamesFStringBackup[i * sizeof(FString)]), &(game->RandomBotNames.at(i)), sizeof(FString));
		memcpy(&(game->RandomBotNames.at(i)), &(config.RandomBotNamesFString[i * sizeof(FString)]), sizeof(FString));
	}
}

static void restoreBotnamesToGameDefault(const ServerConfig &config){
	// replacing TArray is finicky, and FStrings are TArrays, so restore it before problematic uses
	AFoxGame *game = UObject::GetInstanceOf<AFoxGame>(true);
	for(int i = 0;i < NUM_BOT_NAMES;i++){
		memcpy(&(game->RandomBotNames.at(i)), &(config.RandomBotNamesFStringBackup[i * sizeof(FString)]), sizeof(FString));
	}
}

static void logOnMatchIdleHack(){
	logDebug("OnMatchIdle disabled, lobby does not kick players after idling for 3 minutes without starting");
}

static void applyOneshotHacks(const ServerConfig &config){
	if(config.hacks.disableOnMatchIdle){
		// location copied from sdk
		((UFunction*) UObject::GObjObjects()->Data[ 76108 ])->Func = (void *)logOnMatchIdleHack;
	}
}

static void applyRecurringHacks(AFoxGame *game, const ServerConfig &config){
	// place holder
}
