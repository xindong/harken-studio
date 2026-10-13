using NUnit.Framework;
using Harken;

namespace Harken.Unity.Tests
{
    public sealed class HarkenVariableWriteTests
    {
        [Test]
        public void ExplicitEmptyEmitterNeverBecomesGlobalWrite()
        {
            var id = new VnObjectId(0x3da91a64b9474560UL, 7);
            var typed = HarkenVariableId.FromGenerated("Variable", id);
            Assert.That(Values.SetValue(id, 1, default(Emitter)), Is.EqualTo(VnResult.VN_INVALID_ARG));
            Assert.That(Values.SetValue(typed, 1, default(Emitter)), Is.EqualTo(VnResult.VN_INVALID_ARG));
#pragma warning disable CS0618
            Assert.That(default(Emitter).SetValue(id, 1), Is.EqualTo(VnResult.VN_INVALID_ARG));
#pragma warning restore CS0618
        }
    }
}
