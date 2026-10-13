using System;

namespace Harken
{
    [Serializable]
    public struct HarkenVariableId : IEquatable<HarkenVariableId>
    {
        public VnObjectId stableId;
        public string codeName;

        public bool IsAssigned => !stableId.IsNull;

        public static HarkenVariableId FromGenerated(string name, VnObjectId id)
        {
            return new HarkenVariableId
            {
                stableId = id,
                codeName = name ?? string.Empty
            };
        }

        public bool Equals(HarkenVariableId other)
        {
            return stableId == other.stableId &&
                string.Equals(codeName, other.codeName, StringComparison.Ordinal);
        }

        public override bool Equals(object obj)
        {
            return obj is HarkenVariableId other && Equals(other);
        }

        public override int GetHashCode()
        {
            return stableId.GetHashCode();
        }
    }
}
