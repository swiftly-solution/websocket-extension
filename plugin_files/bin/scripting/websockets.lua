local websocketServerCB = {}

AddEventHandler("OnWSServerMessage", function(event, server_uuid, client_id, kind, message)
    if websocketServerCB[server_uuid] then
        websocketServerCB[server_uuid](server_uuid, client_id, kind, message)
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
        if type(message) ~= "string" then message = json.encode(message) or "{}" end

        websocket:SendServerMessageToClient(server_uuid, client_id, message)
    end,

    SendServerMessageToAllClients = function(_, server_uuid, message)
        if not message then message = "" end
        if type(message) ~= "string" then message = json.encode(message) or "{}" end

        websocket:SendServerMessageToAllClients(server_uuid, message)
    end,

    TerminateClientConnectionOnServer = function(_, server_uuid, client_id)
        websocket:TerminateClientConnectionOnServer(server_uuid, client_id)
    end,
}
