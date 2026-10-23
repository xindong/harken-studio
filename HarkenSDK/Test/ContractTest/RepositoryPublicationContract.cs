using System;
using System.IO;
using System.Linq;

namespace Harken.Test
{
    internal static class RepositoryPublicationContract
    {
        public static bool Run()
        {
            string root = Path.Combine(AppContext.BaseDirectory, "NativePublications");
            int count = 0;
            foreach (string category in new[] { "UnityFixture", "PlayableDemo" })
            {
                string directory = Path.Combine(root, category);
                string[] publications = Directory.Exists(directory)
                    ? Directory.GetFiles(directory, "Init.json", SearchOption.AllDirectories)
                    : Array.Empty<string>();
                if (publications.Length == 0)
                {
                    Console.Error.WriteLine("Missing companion publications: " + category);
                    return false;
                }
                foreach (string init in publications.OrderBy(path => path, StringComparer.Ordinal))
                {
                    if (!CompanionReleaseContract.Run(Path.GetDirectoryName(init))) return false;
                    ++count;
                }
            }
            Console.WriteLine("[Repository publications] " + count + " companion publications passed native activation and playback.");
            return true;
        }
    }
}
