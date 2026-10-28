using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;

namespace Harken.Unity.Editor
{
    internal sealed class HarkenVkd
    {
        internal const int ByteLimit = 16 * 1024 * 1024;
        internal uint Rate, Channels, Stride;
        internal ulong Frames;
        internal float[][] Minima, Maxima;
        internal int Count => Minima[0].Length;

        internal static HarkenVkd Read(byte[] bytes, string file)
        {
            if (bytes == null) throw new FileNotFoundException("Published VKD is missing.");
            if (bytes.Length < 44 || bytes.Length > ByteLimit) throw new InvalidDataException("Invalid VKD byte count.");
            using (var sha = SHA256.Create())
            {
                string digest = BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
                if (!string.Equals(Path.GetFileName(file), digest + ".vkd", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("VKD content digest does not match its published filename.");
            }
            using (var stream = new MemoryStream(bytes, false))
            using (var reader = new BinaryReader(stream))
            {
                if (reader.ReadUInt32() != 0x41444b56 || reader.ReadUInt32() != 2) throw new InvalidDataException("Unsupported VKD format.");
                stream.Position = 24;
                uint sections = reader.ReadUInt32();
                if (reader.ReadUInt32() != 0 || sections == 0 || sections > (bytes.Length - 32) / 12)
                    throw new InvalidDataException("Invalid VKD section directory.");
                ulong directoryEnd = 32ul + sections * 12ul;
                uint waveOffset = 0, waveSize = 0;
                var ranges = new List<(uint Start, uint End)>();
                for (uint i = 0; i < sections; ++i)
                {
                    uint type = reader.ReadUInt32(), offset = reader.ReadUInt32(), size = reader.ReadUInt32();
                    if (offset < directoryEnd || (ulong)offset + size > (ulong)bytes.Length) throw new InvalidDataException("VKD section exceeds file bounds.");
                    foreach (var range in ranges)
                        if (offset < range.End && (ulong)offset + size > range.Start) throw new InvalidDataException("Overlapping VKD sections.");
                    ranges.Add((offset, checked(offset + size)));
                    if (type == 0x45564157)
                    {
                        if (waveOffset != 0) throw new InvalidDataException("Duplicate WAVE section.");
                        waveOffset = offset; waveSize = size;
                    }
                }
                if (waveOffset == 0 || waveSize < 24) throw new InvalidDataException("Missing WAVE section.");
                stream.Position = waveOffset;
                uint count = reader.ReadUInt32(), stride = reader.ReadUInt32(), channels = reader.ReadUInt32(), rate = reader.ReadUInt32();
                double duration = reader.ReadDouble();
                if (count == 0 || stride == 0 || channels == 0 || channels > 64 || rate == 0 || rate > 768000 ||
                    24ul + (ulong)count * channels * 8ul != waveSize || !Finite(duration) || duration <= 0 ||
                    duration * rate > 9007199254740991d) throw new InvalidDataException("Invalid WAVE dimensions.");
                ulong frames = checked((ulong)Math.Round(duration * rate));
                if (frames <= (ulong)(count - 1) * stride) throw new InvalidDataException("Invalid WAVE tail bucket.");
                var result = new HarkenVkd { Rate = rate, Channels = channels, Stride = stride, Frames = frames,
                    Minima = new float[channels][], Maxima = new float[channels][] };
                for (int channel = 0; channel < channels; ++channel)
                {
                    result.Minima[channel] = new float[count]; result.Maxima[channel] = new float[count];
                    for (int bucket = 0; bucket < count; ++bucket)
                    {
                        float low = reader.ReadSingle(), high = reader.ReadSingle();
                        if (!Finite(low) || !Finite(high) || low > high) throw new InvalidDataException("Invalid WAVE extrema.");
                        result.Minima[channel][bucket] = low; result.Maxima[channel][bucket] = high;
                    }
                }
                return result;
            }
        }

        internal HarkenSourceEnvelope Trim(VnObjectId sound, ulong start, ulong end, float gain,
            ulong fingerprint, ulong revision)
        {
            if (start >= end || end > Frames || !Finite(gain) || gain < 0) throw new InvalidDataException("Invalid source trim or gain.");
            int first = (int)Math.Min((ulong)Count - 1, start / Stride);
            int last = (int)Math.Min((ulong)Count - 1, (end - 1) / Stride);
            int count = last - first + 1;
            var minima = new float[Channels][]; var maxima = new float[Channels][];
            var boundaries = new ulong[count + 1];
            boundaries[0] = 0; boundaries[count] = end - start;
            for (int b = 1; b < count; ++b) boundaries[b] = (ulong)(first + b) * Stride - start;
            for (int c = 0; c < Channels; ++c)
            {
                minima[c] = new float[count]; maxima[c] = new float[count];
                for (int b = 0; b < count; ++b)
                {
                    float low = Minima[c][first + b] * gain, high = Maxima[c][first + b] * gain;
                    if (!Finite(low) || !Finite(high)) throw new InvalidDataException("Waveform gain exceeds finite range.");
                    minima[c][b] = low; maxima[c][b] = high;
                }
            }
            return new HarkenSourceEnvelope(sound, fingerprint, revision, end - start, Rate, minima, maxima, boundaries);
        }
        private static bool Finite(double value) => !double.IsNaN(value) && !double.IsInfinity(value);
    }
}
