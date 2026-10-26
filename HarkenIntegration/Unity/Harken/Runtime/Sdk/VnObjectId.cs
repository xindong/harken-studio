using System;

namespace Harken
{
    public partial struct VnObjectId : IEquatable<VnObjectId>, IComparable<VnObjectId>
    {
        public VnObjectId(ulong high, ulong low)
        {
            this.high = high;
            this.low = low;
        }

        public bool IsNull => high == 0 && low == 0;

        public static VnObjectId FromGuid(Guid value)
        {
            byte[] bytes = value.ToByteArray();
            ulong high = ((ulong)bytes[3] << 56) | ((ulong)bytes[2] << 48) |
                         ((ulong)bytes[1] << 40) | ((ulong)bytes[0] << 32) |
                         ((ulong)bytes[5] << 24) | ((ulong)bytes[4] << 16) |
                         ((ulong)bytes[7] << 8) | bytes[6];
            ulong low = 0;
            for (int i = 8; i < 16; ++i) low = (low << 8) | bytes[i];
            return new VnObjectId(high, low);
        }

        public Guid ToGuid()
        {
            return new Guid(unchecked((int)(high >> 32)), unchecked((short)(high >> 16)),
                unchecked((short)high), (byte)(low >> 56), (byte)(low >> 48),
                (byte)(low >> 40), (byte)(low >> 32), (byte)(low >> 24),
                (byte)(low >> 16), (byte)(low >> 8), (byte)low);
        }

        public static bool TryParse(string text, out VnObjectId value)
        {
            value = default;
            if (!Guid.TryParseExact(text, "D", out Guid guid) &&
                !Guid.TryParseExact(text, "B", out guid)) return false;
            value = FromGuid(guid);
            return true;
        }

        public static VnObjectId Parse(string text)
        {
            if (!TryParse(text, out VnObjectId value)) throw new FormatException("Invalid object UUID.");
            return value;
        }

        public bool Equals(VnObjectId other) => high == other.high && low == other.low;
        public override bool Equals(object other) => other is VnObjectId id && Equals(id);
        public override int GetHashCode() => unchecked(high.GetHashCode() * 397 ^ low.GetHashCode());
        public int CompareTo(VnObjectId other)
        {
            int order = high.CompareTo(other.high);
            return order == 0 ? low.CompareTo(other.low) : order;
        }

        public override string ToString() => ToGuid().ToString("D");
        public static bool operator ==(VnObjectId left, VnObjectId right) => left.Equals(right);
        public static bool operator !=(VnObjectId left, VnObjectId right) => !left.Equals(right);
    }
}
