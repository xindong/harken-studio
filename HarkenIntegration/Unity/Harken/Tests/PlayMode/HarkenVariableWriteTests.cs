using NUnit.Framework;
using Harken;
using System;
using System.Threading.Tasks;

namespace Harken.Unity.Tests
{
    public sealed class HarkenVariableWriteTests
    {
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
