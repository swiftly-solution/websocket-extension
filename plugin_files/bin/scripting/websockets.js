let websocketServerCB = {}

AddEventHandler("OnWSServerMessage", function (event, server_uuid, client_id, kind, message) {
    if (websocketServerCB.hasOwnProperty(server_uuid)) {
        websocketServerCB[server_uuid](server_uuid, client_id, kind, message)
    }
})

globalThis.ws = {
    StartServer: function (port, cb) {
        const server_uuid = websocket.StartListen(port)
        websocketServerCB[server_uuid] = cb
        return server_uuid
    },

    StopServer: function (server_uuid) {
        websocket.StopListen(server_uuid)

        if (websocketServerCB.hasOwnProperty(server_uuid)) {
            delete websocketServerCB[server_uuid];
        }
    },

    SendServerMessageToClient: function (server_uuid, client_id, message) {
        if (!message) message = "";
        if (typeof message != "string") message = JSON.stringify(message)

        websocket.SendServerMessageToClient(server_uuid, client_id, message)
    },

    SendServerMessageToAllClients: function (server_uuid, message) {
        if (!message) message = "";
        if (typeof message != "string") message = JSON.stringify(message)

        websocket.SendServerMessageToAllClients(server_uuid, message)
    },

    TerminateClientConnectionOnServer: function (server_uuid, client_id) {
        websocket.TerminateClientConnectionOnServer(server_uuid, client_id)
    },
}
