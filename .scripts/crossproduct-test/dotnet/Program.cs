using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using Harken;

internal static class Program
{
    private const int HotIterations = 2048;

    private static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: <native-library-dir> <platform-directory> <action-name>");
            return 2;
        }
        string nativeDirectory = Path.GetFullPath(args[0]);
        string directory = Path.GetFullPath(args[1]);
        string actionName = args[2];
        NativeLibrary.SetDllImportResolver(Assembly.GetExecutingAssembly(), (name, assembly, path) =>
        {
            foreach (string file in new[] { name + ".dll", "lib" + name + ".so", "lib" + name + ".dylib" })
            {
                string candidate = Path.Combine(nativeDirectory, file);
                if (File.Exists(candidate)) return NativeLibrary.Load(candidate);
            }
            return IntPtr.Zero;
        });

        VnConfig config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        Require(PackBootstrap.StartDirectory(directory, null, ref config, out PackBootstrapSession session), "start directory");
        try
        {
            Check(session.PrepareActions(new[] { actionName, "NoSuchAction" }, session.ProjectId,
                out ActionPreparation refused) != VnResult.VN_OK, "a batch with a missing Action is refused");
            Check(refused.IsReleased && Actions.Resolve(actionName, session.ProjectId, out _) != VnResult.VN_OK,
                "a refused batch publishes nothing");

            Require(session.PrepareActions(new[] { actionName }, session.ProjectId, out ActionPreparation preparation),
                "prepare " + actionName);
            Require(Actions.Resolve(actionName, session.ProjectId, out ActionResolved action), "resolve " + actionName);
            Require(HarkenEngine.Update(), "update");
            Require(Actions.Post(in action, Emitter.None, 0, out ActionFiring firing), "post " + actionName);
            Check(firing.IsValid, "post returns a firing");
            Require(firing.Stop(0), "stop");
            Require(HarkenEngine.Update(), "update");

            ActionFiring warm;
            Actions.Post(in action, Emitter.None, 0, out warm);
            warm.Stop(0);
            HarkenEngine.Update();
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < HotIterations; ++i)
            {
                Actions.Post(in action, Emitter.None, (ulong)i, out ActionFiring hot);
                hot.Stop(0);
                HarkenEngine.Update();
            }
            long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Check(allocated == 0, "post, stop and update allocate no managed memory (allocated " + allocated + " bytes)");

            Require(preparation.Release(), "release");
            Require(session.Shutdown(), "shutdown");
            session = null;
            Console.WriteLine("PASS");
            return 0;
        }
        catch (InvalidOperationException error)
        {
            Console.WriteLine("FAIL: " + error.Message);
            return 1;
        }
        finally { session?.Dispose(); }
    }

    private static void Require(VnResult result, string operation)
    {
        if (result != VnResult.VN_OK) throw new InvalidOperationException(operation + ": " + result);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
