using System.IO;
using NUnit.Framework;

namespace Harken.Unity.Tests
{
    public sealed class HarkenReaderCaptureTests
    {
        [Test]
        public void RetiredPublicationCaptureDoesNotInvokeTheReaderOrReturnPartialState()
        {
            int reads = 0;
            Assert.That(HarkenPreparedPacks.Capture("HarkenPackManifest.json", path =>
            {
                ++reads;
                throw new IOException(path);
            }, out var prepared, out var manifest, out _), Is.EqualTo(VnResult.VN_PACK_INCOMPATIBLE));
            Assert.That(reads, Is.Zero);
            Assert.That(prepared, Is.Null);
            Assert.That(manifest, Is.Null);
        }
    }
}
