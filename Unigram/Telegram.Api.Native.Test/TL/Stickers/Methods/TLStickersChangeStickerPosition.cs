// <auto-generated/>
using System;
using Telegram.Api.Native.TL;

namespace Telegram.Api.TL.Stickers.Methods
{
	/// <summary>
	/// RCP method stickers.changeStickerPosition.
	/// Returns <see cref="Telegram.Api.TL.TLBool"/>
	/// </summary>
	public partial class TLStickersChangeStickerPosition : TLObject
	{
		public TLInputDocumentBase Sticker { get; set; }
		public Int32 Position { get; set; }

		public TLStickersChangeStickerPosition() { }
		public TLStickersChangeStickerPosition(TLBinaryReader from)
		{
			Read(from);
		}

		public override TLType TypeId { get { return TLType.StickersChangeStickerPosition; } }

		public override void Read(TLBinaryReader from)
		{
			Sticker = TLFactory.Read<TLInputDocumentBase>(from);
			Position = from.ReadInt32();
		}

		public override void Write(TLBinaryWriter to)
		{
			to.WriteObject(Sticker);
			to.WriteInt32(Position);
		}
	}
}