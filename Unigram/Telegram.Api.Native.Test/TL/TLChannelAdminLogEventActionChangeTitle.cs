// <auto-generated/>
using System;
using Telegram.Api.Native.TL;

namespace Telegram.Api.TL
{
	public partial class TLChannelAdminLogEventActionChangeTitle : TLChannelAdminLogEventActionBase 
	{
		public String PrevValue { get; set; }
		public String NewValue { get; set; }

		public TLChannelAdminLogEventActionChangeTitle() { }
		public TLChannelAdminLogEventActionChangeTitle(TLBinaryReader from)
		{
			Read(from);
		}

		public override TLType TypeId { get { return TLType.ChannelAdminLogEventActionChangeTitle; } }

		public override void Read(TLBinaryReader from)
		{
			PrevValue = from.ReadString();
			NewValue = from.ReadString();
		}

		public override void Write(TLBinaryWriter to)
		{
			to.WriteString(PrevValue ?? string.Empty);
			to.WriteString(NewValue ?? string.Empty);
		}
	}
}