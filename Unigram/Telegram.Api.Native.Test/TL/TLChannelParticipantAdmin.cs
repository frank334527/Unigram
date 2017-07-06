// <auto-generated/>
using System;
using Telegram.Api.Native.TL;

namespace Telegram.Api.TL
{
	public partial class TLChannelParticipantAdmin : TLChannelParticipantBase 
	{
		[Flags]
		public enum Flag : Int32
		{
			CanEdit = (1 << 0),
		}

		public bool IsCanEdit { get { return Flags.HasFlag(Flag.CanEdit); } set { Flags = value ? (Flags | Flag.CanEdit) : (Flags & ~Flag.CanEdit); } }

		public Flag Flags { get; set; }
		public Int32 InviterId { get; set; }
		public Int32 PromotedBy { get; set; }
		public Int32 Date { get; set; }
		public TLChannelAdminRights AdminRights { get; set; }

		public TLChannelParticipantAdmin() { }
		public TLChannelParticipantAdmin(TLBinaryReader from)
		{
			Read(from);
		}

		public override TLType TypeId { get { return TLType.ChannelParticipantAdmin; } }

		public override void Read(TLBinaryReader from)
		{
			Flags = (Flag)from.ReadInt32();
			UserId = from.ReadInt32();
			InviterId = from.ReadInt32();
			PromotedBy = from.ReadInt32();
			Date = from.ReadInt32();
			AdminRights = TLFactory.Read<TLChannelAdminRights>(from);
		}

		public override void Write(TLBinaryWriter to)
		{
			to.WriteInt32((Int32)Flags);
			to.WriteInt32(UserId);
			to.WriteInt32(InviterId);
			to.WriteInt32(PromotedBy);
			to.WriteInt32(Date);
			to.WriteObject(AdminRights);
		}
	}
}