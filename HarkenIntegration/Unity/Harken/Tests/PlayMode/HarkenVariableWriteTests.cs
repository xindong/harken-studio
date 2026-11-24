using NUnit.Framework;
using Harken;
using System;
using System.Threading.Tasks;

namespace Harken.Unity.Tests
{
    public sealed class HarkenVariableWriteTests
    {
#if UNITY_EDITOR
        [Test]
        public void LabelAndNumericWritesTargetOnlyTheirOwnSpatialSources()
        {
            var package = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(HarkenSpatialSource).Assembly);
            var directory = System.IO.Path.Combine(package.resolvedPath, "Tests/Fixtures~/SpatialValues");
            var config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            Assert.That(PackBootstrap.StartDirectory(directory, "", ref config, out var session), Is.EqualTo(VnResult.VN_OK));

            using (session)
            {
                var first = new UnityEngine.GameObject("First source");
                var second = new UnityEngine.GameObject("Second source");
                try
                {
                    var sources = new HarkenSpatialSources();
                    Assert.That(sources.Create(this, session, first.transform, out var a), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(sources.Create(this, session, second.transform, out var b), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(a.Validate(this, session, out var firstEmitter), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(b.Validate(this, session, out var secondEmitter), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(Values.Resolve("View", out var id), Is.EqualTo(VnResult.VN_OK));

                    Assert.That(a.SetValue("View", "Far"), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HasWrite(id, firstEmitter.Handle), Is.True);
                    Assert.That(HasWrite(id, secondEmitter.Handle), Is.False);
                    Assert.That(HasWrite(id, 0), Is.False);

                    Assert.That(b.SetValue("View", 1f), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HasWrite(id, secondEmitter.Handle), Is.True);
                    Assert.That(HasWrite(id, 0), Is.False);
                }
                finally
                {
                    UnityEngine.Object.DestroyImmediate(first);
                    UnityEngine.Object.DestroyImmediate(second);
                }
            }
        }

        private static bool HasWrite(VnObjectId id, ulong emitter)
        {
            // No device clock is required: the query exposes pending and applied writes.
            var query = new VnVariableQueryDescV1
            {
                structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryDescV1>(),
                variableId = id,
                emitter = emitter
            };
            var result = new VnVariableQueryResultV1
            {
                structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryResultV1>()
            };
            Assert.That(Values.QueryVariable(in query, ref result), Is.EqualTo(VnResult.VN_OK));
            return result.pendingCount != 0 || result.hasEmitterOverride != 0 || result.currentValue != result.defaultValue;
        }
#endif

        [Test]
        public void ExplicitEmptyEmitterNeverBecomesGlobalWrite()
        {
            var id = new VnObjectId(0x3da91a64b9474560UL, 7);
            Assert.That(Values.SetValue(id, 1, default(Emitter)), Is.EqualTo(VnResult.VN_INVALID_ARG));
        }

        [Test]
        public void UnavailableSpatialSourceRejectsLocalWrites()
        {
            var session = new PackBootstrapSession(default, Array.Empty<ulong>(), default, "", "", null);
            var source = new HarkenSpatialSource(new object(), session, null);
            Assert.That(source.SetValue("Perspective", "ThirdPerson"), Is.EqualTo(VnResult.VN_STALE_HANDLE));
            Assert.That(source.SetValue("Perspective", 1f), Is.EqualTo(VnResult.VN_STALE_HANDLE));
            Assert.That(Task.Run(() => source.SetValue("Perspective", 1f)).GetAwaiter().GetResult(),
                Is.EqualTo(VnResult.VN_INVALID_ARG));
        }
    }
}
