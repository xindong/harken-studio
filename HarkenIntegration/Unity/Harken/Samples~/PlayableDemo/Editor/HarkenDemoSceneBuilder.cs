using System;

namespace Harken.Samples.PlayableDemo.Editor
{
    public static class HarkenDemoSceneBuilder
    {
        public static void Rebuild()
        {
            throw new InvalidOperationException(HarkenPlayableDemoController.UnavailableReason);
        }

        public static void RebuildAtPath(string root)
        {
            throw new InvalidOperationException(HarkenPlayableDemoController.UnavailableReason);
        }
    }
}
