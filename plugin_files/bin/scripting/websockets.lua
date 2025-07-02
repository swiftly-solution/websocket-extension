local websocketServerCB = {}
local websocketClientCB = {}

AddEventHandler("OnWSServerMessage", function(event, server_uuid, client_id, kind, message)
    if websocketServerCB[server_uuid] then
        websocketServerCB[server_uuid](server_uuid, client_id, kind, message)
    end
end)

AddEventHandler("OnWSClientMessage", function(event, connection_uuid, kind, message)
    if websocketClientCB[connection_uuid] then
        websocketClientCB[connection_uuid](connection_uuid, kind, message)
    end
end)

ws = {
    StartServer = function(_, port, cb)
        local server_uuid = websocket:StartListen(port)
        websocketServerCB[server_uuid] = cb
        return server_uuid
    end,

    StopServer = function(_, server_uuid)
        websocket:StopListen(server_uuid)

        if websocketServerCB[server_uuid] then
            websocketServerCB[server_uuid] = nil
        end
    end,

    SendServerMessageToClient = function(_, server_uuid, client_id, message)
        if not message then message = "" end
        if type(message) ~= "string" then message = (type(message) == "table" and (json.encode(message) or "{}") or tostring(message)) end

        websocket:SendServerMessageToClient(server_uuid, client_id, message)
    end,

    SendServerMessageToAllClients = function(_, server_uuid, message)
        if not message then message = "" end
        if type(message) ~= "string" then message = (type(message) == "table" and (json.encode(message) or "{}") or tostring(message)) end

        websocket:SendServerMessageToAllClients(server_uuid, message)
    end,

    TerminateClientConnectionOnServer = function(_, server_uuid, client_id)
        websocket:TerminateClientConnectionOnServer(server_uuid, client_id)
    end,

    ConnectToServer = function(_, dns, port, isSecure, path, callback)
        local connection_uuid = websocket:ConnectToServer(dns, port, isSecure, path)
        websocketClientCB[connection_uuid] = callback
        return connection_uuid
    end,

    StopConnectionToServer = function(_, connection_uuid)
        websocket:DisconnectFromServer(connection_uuid)
        if websocketClientCB[connection_uuid] then
            websocketClientCB[connection_uuid] = nil
        end
    end,

    SendMessageToServer = function(_, connection_uuid, message)
        if not message then message = "" end
        if type(message) ~= "string" then message = (type(message) == "table" and (json.encode(message) or "{}") or tostring(message)) end

        websocket:SendMessageToServer(connection_uuid, message)
    end
}
