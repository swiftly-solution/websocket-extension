using SwiftlyS2.Internal_API;

namespace SwiftlyS2.API.Extensions
{
    public class WebSockets
    {
        private static Dictionary<string, Action<string, ulong, string, string>> _servercallbacks = [];
        private static Dictionary<string, Action<string, string, string>> _clientcallbacks = [];

        private static IntPtr _ctx = IntPtr.Zero;

        private static void InitializeContext()
        {
            if (_ctx != IntPtr.Zero) return;
            _ctx = Invoker.CallNative<IntPtr>("WS", "WS", CallKind.ClassFunction);

            Scripting.Events.AddEventHandler("OnWSServerMessage", (Scripting.Events.Event e, string server_uuid, ulong client_id, string kind, string message) =>
            {
                if (_servercallbacks.ContainsKey(server_uuid))
                {
                    _servercallbacks[server_uuid](server_uuid, client_id, kind, message);
                }
            });

            Scripting.Events.AddEventHandler("OnWSServerMessage", (Scripting.Events.Event e, string connection_uuid, string kind, string message) =>
            {
                if (_clientcallbacks.ContainsKey(connection_uuid))
                {
                    _clientcallbacks[connection_uuid](connection_uuid, kind, message);
                }
            });
        }

        public static string StartServer(int port, Action<string, ulong, string, string> callback)
        {
            InitializeContext();

            var callbackUUID = Guid.NewGuid().ToString();
            while (_servercallbacks.ContainsKey(callbackUUID))
            {
                callbackUUID = Guid.NewGuid().ToString();
            }

            _servercallbacks.Add(callbackUUID, callback);

            Internal_API.Invoker.CallNative("WS", "StartServer", Internal_API.CallKind.ClassFunction, _ctx, port, callbackUUID);
            return callbackUUID;
        }

        public static void StopServer(string server_uuid)
        {
            InitializeContext();
            Internal_API.Invoker.CallNative("WS", "StopServer", Internal_API.CallKind.ClassFunction, _ctx, server_uuid);
            if(_servercallbacks.ContainsKey(server_uuid)) _servercallbacks.Remove(server_uuid);
        }

        public static void SendServerMessageToClient(string server_uuid, ulong client_id, string message)
        {
            InitializeContext();
            Internal_API.Invoker.CallNative("WS", "SendServerMessageToClient", Internal_API.CallKind.ClassFunction, _ctx, server_uuid, client_id, message);
        }

        public static void SendServerMessageToAllClients(string server_uuid, string message)
        {
            InitializeContext();
            Internal_API.Invoker.CallNative("WS", "SendServerMessageToAllClients", Internal_API.CallKind.ClassFunction, _ctx, server_uuid, message);
        }

        public static void TerminateClientConnectionOnServer(string server_uuid, ulong client_id)
        {
            InitializeContext();
            Internal_API.Invoker.CallNative("WS", "TerminateClientConnectionOnServer", Internal_API.CallKind.ClassFunction, _ctx, server_uuid, client_id);
        }
        public static string ConnectToServer(string dns, int port, bool isSecure, string path, Action<string, string, string> callback)
        {
            InitializeContext();

            string connection = Internal_API.Invoker.CallNative<string>("WS", "ConnectToServer", Internal_API.CallKind.ClassFunction, _ctx, dns, port, isSecure, path) ?? "";
            _clientcallbacks.Add(connection, callback);
            return connection;
        }
        public static void StopConnectionToServer(string connection_uuid)
        {
            InitializeContext();

            Internal_API.Invoker.CallNative("WS", "DisconnectFromServer", Internal_API.CallKind.ClassFunction, _ctx, connection_uuid);
            if(_clientcallbacks.ContainsKey(connection_uuid)) _clientcallbacks.Remove(connection_uuid);
        }

        public static void SendMessageToServer(string connection_uuid, string message)
        {
            InitializeContext();
            Internal_API.Invoker.CallNative("WS", "SendMessageToServer", Internal_API.CallKind.ClassFunction, _ctx, connection_uuid, message);
        }
    }
}