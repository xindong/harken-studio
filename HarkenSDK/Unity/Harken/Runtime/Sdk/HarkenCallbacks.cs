using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Harken
{
    /// <summary>
    /// AOT-safe callback bridge for <c>VnConfig.onNotification</c> /
    /// <c>VnConfig.logCallback</c>. The managed->native function pointers are
    /// created ONCE: static delegate instances held alive plus their cached
    /// function pointers. The static thunks look the user handler up by a
    /// <c>userData</c> registry key. So there is no per-call delegate/closure
    /// allocation and no runtime code generation, which is what IL2CPP/AOT
    /// requires. Per the C ABI, both callbacks fire only inside <c>Vn_Update</c>
    /// on the pumping thread, never on the audio thread, and the API is
    /// externally synchronized, so the registry needs no locking.
    /// </summary>
    public static class HarkenCallbacks
    {
        public sealed class Registration : IDisposable
        {
            private int key;
            private readonly bool notification;

            internal Registration(int key, bool notification)
            {
                this.key = key;
                this.notification = notification;
            }

            public bool IsActive => key != 0;

            public void Dispose()
            {
                int current = key;
                if (current == 0)
                {
                    return;
                }
                key = 0;
                if (notification)
                {
                    NotifySinks.Remove(current);
                }
                else
                {
                    LogSinks.Remove(current);
                }
            }
        }

        public delegate void NotificationHandler(in VnNotification notification, object state);

        public delegate void LogHandler(uint level, uint code, string text, object state);

        // Held alive for process lifetime so the native function pointers stay
        // valid; created once, never per call.
        private static readonly VnNotificationCallback NotifyDelegate = NotifyThunk;
        private static readonly VnLogFn LogDelegate = LogThunk;

        /// <summary>The stable managed->native pointer for the notification thunk.</summary>
        internal static readonly IntPtr NotificationFnPtr =
            Marshal.GetFunctionPointerForDelegate(NotifyDelegate);

        /// <summary>The stable managed->native pointer for the log thunk.</summary>
        internal static readonly IntPtr LogFnPtr =
            Marshal.GetFunctionPointerForDelegate(LogDelegate);

        private struct NotifySink
        {
            public NotificationHandler Handler;
            public object State;
        }

        private struct LogSink
        {
            public LogHandler Handler;
            public object State;
        }

        private static readonly Dictionary<int, NotifySink> NotifySinks = new Dictionary<int, NotifySink>();
        private static readonly Dictionary<int, LogSink> LogSinks = new Dictionary<int, LogSink>();
        private static int _nextKey = 1;

        /// <summary>
        /// Installs a notification handler into <paramref name="config"/> before
        /// <c>Vn_Init</c>: sets <c>onNotification</c> to the cached static thunk
        /// pointer and <c>notificationUserData</c> to a registry key. Keep the SDK
        /// loaded for the engine's lifetime.
        /// </summary>
        public static Registration InstallNotification(ref VnConfig config, NotificationHandler handler, object state = null)
        {
            if (handler == null)
            {
                throw new ArgumentNullException(nameof(handler));
            }
            int key = _nextKey++;
            NotifySinks[key] = new NotifySink { Handler = handler, State = state };
            config.onNotification = NotificationFnPtr;
            config.notificationUserData = (IntPtr)key;
            return new Registration(key, true);
        }

        /// <summary>Installs a log handler into <paramref name="config"/> before <c>Vn_Init</c>.</summary>
        public static Registration InstallLog(ref VnConfig config, LogHandler handler, object state = null)
        {
            if (handler == null)
            {
                throw new ArgumentNullException(nameof(handler));
            }
            int key = _nextKey++;
            LogSinks[key] = new LogSink { Handler = handler, State = state };
            config.logCallback = LogFnPtr;
            config.logUserData = (IntPtr)key;
            return new Registration(key, false);
        }

        [AOT.MonoPInvokeCallback(typeof(VnNotificationCallback))]
        private static unsafe void NotifyThunk(IntPtr notification, IntPtr userData)
        {
            if (!NotifySinks.TryGetValue((int)userData, out NotifySink sink) || sink.Handler == null)
            {
                return;
            }
            ref VnNotification n = ref *(VnNotification*)notification.ToPointer();
            sink.Handler(in n, sink.State);
        }

        [AOT.MonoPInvokeCallback(typeof(VnLogFn))]
        private static void LogThunk(IntPtr userData, uint level, uint code, IntPtr text)
        {
            if (!LogSinks.TryGetValue((int)userData, out LogSink sink) || sink.Handler == null)
            {
                return;
            }
            string message = text == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(text);
            sink.Handler(level, code, message, sink.State);
        }
    }
}
