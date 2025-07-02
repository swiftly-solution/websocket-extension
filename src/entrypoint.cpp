#include "entrypoint.h"
#include <embedder/src/Embedder.h>
#include <random>
#include <map>
#include <string>
#include <set>
#include <thread>
#include <civetweb/civetweb.h>
#include <swiftly-ext/event.h>
#include <atomic>

SH_DECL_HOOK1_void(IServerGameDLL, PreWorldUpdate, SH_NOATTRIB, 0, bool);

//////////////////////////////////////////////////////////////
/////////////////        Core Variables        //////////////
////////////////////////////////////////////////////////////

struct ClientInfo
{
    uint64_t connectionNumber;
};

struct ServerInfo
{
    char* port;
    char* uuid;
};

static const char subprotocol_bin[] = "Company.ProtoName.bin";
static const char subprotocol_json[] = "Company.ProtoName.json";
static const char* subprotocols[] = { subprotocol_bin, subprotocol_json, NULL };
static struct mg_websocket_subprotocols wsprot = { 2, subprotocols };

WSExtension g_Ext;
CREATE_GLOBALVARS();

std::map<std::string, struct mg_context*> internalWSServer;
std::map<std::string, std::map<uint64_t, struct mg_connection*>> wsConnections;

ISource2Server* server = nullptr;

//////////////////////////////////////////////////////////////
/////////////////          Core Class          //////////////
////////////////////////////////////////////////////////////

int32_t genrand()
{
    static std::random_device rd;
    static std::mt19937 rng(rd());
    return std::uniform_int_distribution<int>(0, INT_MAX)(rng);
}

std::string get_uuid()
{
    return string_format(
        "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
        (genrand() & 0xFFFF), (genrand() & 0xFFFF),
        (genrand() & 0xFFFF),
        ((genrand() & 0x0fff) | 0x4000),
        (genrand() % 0x3fff + 0x8000),
        (genrand() & 0xFFFF), (genrand() & 0xFFFF), (genrand() & 0xFFFF));
}

EXT_EXPOSE(g_Ext);
bool WSExtension::Load(std::string& error, SourceHook::ISourceHook* SHPtr, ISmmAPI* ismm, bool late)
{
    mg_init_library(MG_FEATURES_SSL);
    SAVE_GLOBALVARS();

    GET_IFACE_ANY(GetServerFactory, server, ISource2Server, INTERFACEVERSION_SERVERGAMEDLL);

    SH_ADD_HOOK_MEMFUNC(IServerGameDLL, PreWorldUpdate, server, this, &WSExtension::Hook_PreWorldUpdate, true);

    return true;
}

bool WSExtension::Unload(std::string& error)
{
    for (auto it = internalWSServer.begin(); it != internalWSServer.end(); ++it) {
        mg_stop(it->second);
    }

    SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, PreWorldUpdate, server, this, &WSExtension::Hook_PreWorldUpdate, true);

    mg_exit_library();
    return true;
}

void WSExtension::Hook_PreWorldUpdate(bool simulating)
{
    while (!m_nextFrame.empty())
    {
        auto pair = m_nextFrame.front();
        pair.first(pair.second);
        m_nextFrame.pop_front();
    }
}

void WSExtension::NextFrame(std::function<void(std::vector<std::any>)> fn, std::vector<std::any> param)
{
    m_nextFrame.push_back({ fn, param });
}

void WSExtension::AllExtensionsLoaded()
{

}

void WSExtension::AllPluginsLoaded()
{

}

void WSServerMessage(std::vector<std::any> args)
{
    std::string server_id = std::any_cast<char*>(args[0]);
    uint64_t client_id = std::any_cast<uint64_t>(args[1]);
    std::string kind = std::any_cast<const char*>(args[2]);
    std::string message = std::any_cast<std::string>(args[3]);
    bool* finished = std::any_cast<bool*>(args[4]);

    std::any ret;
    TriggerEvent("websockets.ext", "OnWSServerMessage", { server_id, client_id, kind, message }, ret);
    *finished = true;
}

static int ws_connect_handler(const struct mg_connection* conn, void* user_data)
{
    struct ClientInfo* wsCliCtx = (struct ClientInfo*)calloc(1, sizeof(struct ClientInfo));
    if (!wsCliCtx) {
        return 1;
    }

    static std::atomic<uint64_t> connectionCounter{ 0 };
    wsCliCtx->connectionNumber = connectionCounter.fetch_add(1) + 1;
    mg_set_user_connection_data(conn, wsCliCtx);

    std::string uid = (char*)user_data;
    if (wsConnections.find(uid) == wsConnections.end()) wsConnections[uid] = std::map<uint64_t, struct mg_connection*>{};
    wsConnections[uid].insert({ wsCliCtx->connectionNumber, (struct mg_connection*)conn });

    const struct mg_request_info* ri = mg_get_request_info(conn);

    bool finished = false;
    g_Ext.NextFrame(WSServerMessage, { (char*)user_data, wsCliCtx->connectionNumber, "open", std::string(""), &finished });
    while (!finished) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return 0;
}

static void ws_ready_handler(struct mg_connection* conn, void* user_data)
{
    struct ClientInfo* wsCliCtx = (struct ClientInfo*)mg_get_user_connection_data(conn);

    bool finished = false;
    g_Ext.NextFrame(WSServerMessage, { (char*)user_data, wsCliCtx->connectionNumber, "ready", std::string(""), &finished });
    while (!finished) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
}

static int ws_data_handler(struct mg_connection* conn, int opcode, char* data, size_t datasize, void* user_data)
{
    struct ClientInfo* wsCliCtx = (struct ClientInfo*)mg_get_user_connection_data(conn);

    if ((opcode & 0xf) != MG_WEBSOCKET_OPCODE_TEXT) return 1;

    bool finished = false;
    g_Ext.NextFrame(WSServerMessage, { (char*)user_data, wsCliCtx->connectionNumber, "message", std::string(data, datasize), &finished });
    while (!finished) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return 1;
}

static void ws_close_handler(const struct mg_connection* conn, void* user_data)
{
    (void)user_data;

    struct ClientInfo* wsCliCtx = (struct ClientInfo*)mg_get_user_connection_data(conn);

    bool finished = false;
    g_Ext.NextFrame(WSServerMessage, { (char*)user_data, wsCliCtx->connectionNumber, "close", std::string(""), &finished });
    while (!finished) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    std::string uid = (char*)user_data;
    if (wsConnections.find(uid) == wsConnections.end()) wsConnections[uid] = std::map<uint64_t, struct mg_connection*>{};
    wsConnections[uid].erase(wsCliCtx->connectionNumber);

    free(wsCliCtx);
}


bool WSExtension::OnPluginLoad(std::string pluginName, void* pluginState, PluginKind_t kind, std::string& error)
{
    EContext* ctx = (EContext*)pluginState;

    ADD_CLASS("WS");

    ADD_CLASS_FUNCTION("WS", "~WS", [](FunctionContext* context, ClassData* data) -> void {
        if (data->HasData("server_uuids")) {
            auto vec = data->GetData<std::vector<std::string>>("server_uuids");
            for (std::string uuid : vec) {
                mg_stop(internalWSServer[uuid]);
                internalWSServer.erase(uuid);
            }
        }
        });

    ADD_CLASS_FUNCTION("WS", "StartListen", [](FunctionContext* context, ClassData* data) -> void {
        int port = context->GetArgumentOr<int>(0, 1337);
        if (port > 65535) return;

        struct ServerInfo* serverCtx = (struct ServerInfo*)malloc(sizeof(struct ServerInfo));

        std::string uuid = get_uuid();
        char* uid = strdup(uuid.c_str());

        serverCtx->port = strdup(std::to_string(port).c_str());
        serverCtx->uuid = uid;

        const char* SERVER_OPTIONS[] = {
            "listening_ports", serverCtx->port,
            nullptr, nullptr,
        };

        struct mg_callbacks callbacks = { 0 };

        struct mg_init_data mg_start_init_data = { 0 };
        mg_start_init_data.callbacks = &callbacks;
        mg_start_init_data.user_data = uid;
        mg_start_init_data.configuration_options = SERVER_OPTIONS;

        struct mg_error_data mg_start_error_data = { 0 };
        char errtxtbuf[256] = { 0 };
        mg_start_error_data.text = errtxtbuf;
        mg_start_error_data.text_buffer_size = sizeof(errtxtbuf);

        struct mg_context* ctx = mg_start2(&mg_start_init_data, &mg_start_error_data);
        if (!ctx) {
            printf("[Websockets] An error has occured while trying to start WS Server: %s\n", errtxtbuf);
            return context->SetReturn("00000000-0000-0000-0000-000000000000");
        }

        mg_set_websocket_handler_with_subprotocols(ctx, "/", &wsprot, ws_connect_handler, ws_ready_handler, ws_data_handler, ws_close_handler, uid);

        internalWSServer[uuid] = ctx;

        auto vec = data->GetDataOr<std::vector<std::string>>("server_uuids", std::vector<std::string>{});
        vec.push_back(uuid);
        data->SetData("server_uuids", vec);

        context->SetReturn(uuid);

        });

    ADD_CLASS_FUNCTION("WS", "StopListen", [](FunctionContext* context, ClassData* data) -> void {
        std::string server_uuid = context->GetArgumentOr<std::string>(0, "0");

        auto vec = data->GetDataOr<std::vector<std::string>>("server_uuids", std::vector<std::string>{});
        auto it = std::find(vec.begin(), vec.end(), server_uuid);
        if (it == vec.end()) return;

        vec.erase(it);
        data->SetData("server_uuids", vec);

        ServerInfo* ctx = (ServerInfo*)mg_get_user_data(internalWSServer[server_uuid]);
        free(ctx->port);
        free(ctx->uuid);
        free(ctx);

        mg_stop(internalWSServer[server_uuid]);

        internalWSServer.erase(server_uuid);

        });

    ADD_CLASS_FUNCTION("WS", "SendServerMessageToClient", [](FunctionContext* context, ClassData* data) -> void {
        std::string server_uuid = context->GetArgumentOr<std::string>(0, "0");
        uint64_t client_id = context->GetArgumentOr<uint64_t>(1, 0);
        std::string message = context->GetArgumentOr<std::string>(2, "");

        if (wsConnections.find(server_uuid) == wsConnections.end()) wsConnections[server_uuid] = std::map<uint64_t, struct mg_connection*>{};
        if (wsConnections[server_uuid].find(client_id) != wsConnections[server_uuid].end()) {
            auto conn = wsConnections[server_uuid][client_id];
            mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, message.c_str(), message.size());
        }
        });

    ADD_CLASS_FUNCTION("WS", "SendServerMessageToAllClients", [](FunctionContext* context, ClassData* data) -> void {
        std::string server_uuid = context->GetArgumentOr<std::string>(0, "0");
        std::string message = context->GetArgumentOr<std::string>(1, "");

        if (wsConnections.find(server_uuid) == wsConnections.end()) wsConnections[server_uuid] = std::map<uint64_t, struct mg_connection*>{};
        for (auto it = wsConnections[server_uuid].begin(); it != wsConnections[server_uuid].end(); ++it) {
            mg_websocket_write(it->second, MG_WEBSOCKET_OPCODE_TEXT, message.c_str(), message.size());
        }
        });

    ADD_CLASS_FUNCTION("WS", "TerminateClientConnectionOnServer", [](FunctionContext* context, ClassData* data) -> void {
        std::string server_uuid = context->GetArgumentOr<std::string>(0, "0");
        uint64_t client_id = context->GetArgumentOr<uint64_t>(1, 0);

        if (wsConnections.find(server_uuid) == wsConnections.end()) wsConnections[server_uuid] = std::map<uint64_t, struct mg_connection*>{};
        if (wsConnections[server_uuid].find(client_id) != wsConnections[server_uuid].end()) {
            mg_websocket_write(wsConnections[server_uuid][client_id], MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, "", 0);
            mg_close_connection(wsConnections[server_uuid][client_id]);
        }
        });

    ADD_VARIABLE("_G", "websocket", MAKE_CLASS_INSTANCE_CTX(ctx, "WS", {}));

    return true;
}

bool WSExtension::OnPluginUnload(std::string pluginName, void* pluginState, PluginKind_t kind, std::string& error)
{
    return true;
}

const char* WSExtension::GetAuthor()
{
    return "Swiftly Development Team";
}

const char* WSExtension::GetName()
{
    return "WebSocket Extension";
}

const char* WSExtension::GetVersion()
{
#ifndef VERSION
    return "Local";
#else
    return VERSION;
#endif
}

const char* WSExtension::GetWebsite()
{
    return "https://swiftlys2.net/";
}
