using System;
using System.Reflection;
using Harken;

namespace Harken.Test
{
    internal static class VariableApiContract
    {
        public static bool Run()
        {
            try
            {
                VnObjectId vehicleRpm = VnObjectId.Parse("67e0c65b-f36e-40d9-80ee-75af00ecf712");
                var labelId = new VnObjectId(0, 721);
                Check(Values.SetValue(vehicleRpm, "class", default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "label write with explicit empty emitter never selects global");
                Check(Values.SetValue(vehicleRpm, labelId, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "label identity write with explicit empty emitter never selects global");
                Check(Values.SetValue(vehicleRpm, "bad\0name") == VnResult.VN_INVALID_ARG,
                    "embedded null label names refused");
                Check(Values.SetValue(vehicleRpm, 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "raw explicit empty emitter never selects global");
                Check(typeof(Emitter).GetMethod("SetValue") == null &&
                    typeof(Emitter).GetMethod("SetValues") == null, "id-only emitter setters are not public");
                Check(Values.SetValue("VehicleRPM", 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "named explicit empty emitter never selects global");
                Check(Values.Resolve("Vehicle\0RPM", out _) == VnResult.VN_INVALID_ARG,
                    "embedded NUL cannot truncate a Variable name");
                Check(Values.Resolve("", out _) == VnResult.VN_INVALID_ARG, "empty Variable name refused");
                Check(typeof(Emitter).GetMethod("ClearValue") == null &&
                    typeof(Emitter).GetMethod("ClearValues") == null, "local clear is not public");
                Check(typeof(Values).GetMethod("SetValue",
                    new[] { typeof(VnObjectId), typeof(float), typeof(Emitter?) }) != null,
                    "GUID SetValue remains public");
                MethodInfo unified = typeof(Values).GetMethod("SetValue",
                    new[] { typeof(string), typeof(string), typeof(float), typeof(Emitter?) });
                Check(unified != null && unified.GetParameters()[3].IsOptional, "name and project SetValue");
                Check(typeof(Actions).GetMethod("Post",
                    new[] { typeof(string), typeof(Emitter), typeof(ActionFiring).MakeByRefType() }) != null,
                    "Post(string) remains public");
                VariableGameplay.GameThreadProbe = () => false;
                Check(Values.SetValue(vehicleRpm, 1) == VnResult.VN_INVALID_ARG,
                    "thread guard precedes native entry");
                Check(Values.SetValue(vehicleRpm, labelId) == VnResult.VN_INVALID_ARG,
                    "label identity thread guard precedes native entry");
                Check(Values.SetValue(vehicleRpm, "class") == VnResult.VN_INVALID_ARG,
                    "label name thread guard precedes native entry");
                Console.WriteLine("[Variable API] optional emitter and thread contracts PASS");
                return true;
            }
            catch (InvalidOperationException error)
            {
                Console.WriteLine("[Variable API] FAIL: " + error);
                return false;
            }
            finally
            {
                VariableGameplay.GameThreadProbe = null;
            }
        }

        private static void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
        }
    }
}
