// This file was generated by the Gtk# code generator.
// Any changes made will be lost if regenerated.

namespace Gst {

	using System;
	using System.Runtime.InteropServices;

#region Autogenerated code
	[Flags]
	[GLib.GType (typeof (Gst.SerializeFlagsGType))]
	public enum SerializeFlags : uint {

		None = 0,
		BackwardCompat = 1,
		Strict = 2,
	}

	internal class SerializeFlagsGType {
		[DllImport ("gstreamer-1.0-0.dll", CallingConvention = CallingConvention.Cdecl)]
		static extern IntPtr gst_serialize_flags_get_type ();

		public static GLib.GType GType {
			get {
				return new GLib.GType (gst_serialize_flags_get_type ());
			}
		}
	}
#endregion
}
