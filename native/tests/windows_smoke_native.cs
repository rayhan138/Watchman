// Small external Win32 driver for windows_smoke_test.ps1. No app hooks,
// injected DLLs, mocked window procedures or special Watchman build required.
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class WatchmanSmokeNative {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    private delegate bool EnumCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder text, int maximum);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int maximum);
    [DllImport("user32.dll")] public static extern bool IsWindowUnicode(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, StringBuilder lparam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr window, int index);
    [DllImport("user32.dll")] private static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] private static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetMenuString(IntPtr menu, uint index, StringBuilder text, int maximum, uint flags);
    [DllImport("user32.dll")] public static extern bool GetMenuItemRect(IntPtr owner, IntPtr menu, uint index, out Rect rect);
    [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr menu, int index);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr menu, int index);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr menu, uint index, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] public static extern bool InvalidateRect(IntPtr window, IntPtr rect, bool erase);
    [DllImport("dwmapi.dll")] public static extern int DwmFlush();

    public static IntPtr Find(string className, int process) {
        IntPtr found = IntPtr.Zero;
        EnumCallback callback = (window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            StringBuilder text = new StringBuilder(256);
            GetClassName(window, text, text.Capacity);
            if ((process == 0 || owner == process) && text.ToString() == className) { found = window; return false; }
            return true;
        };
        EnumWindows(callback, IntPtr.Zero);
        if (found == IntPtr.Zero) EnumChildWindows(GetDesktopWindow(), callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return found;
    }
    // Sampling supplements deterministic owned-window fault tests; it is not
    // a physical-pixel or actual sign-in measurement.
    public static bool StaysHidden(IntPtr window, int milliseconds) {
        var clock = System.Diagnostics.Stopwatch.StartNew();
        do {
            if (!IsWindow(window) || IsWindowVisible(window) || GetForegroundWindow() == window) return false;
            Thread.Sleep(10);
        } while (clock.ElapsedMilliseconds < milliseconds);
        return true;
    }
    public static string Caption(IntPtr window) {
        StringBuilder text = new StringBuilder(1024);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
    public static string DescribeWindows(int process) {
        StringBuilder result = new StringBuilder();
        EnumCallback callback = (window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != process) return true;
            StringBuilder name = new StringBuilder(256);
            GetClassName(window, name, name.Capacity);
            result.AppendFormat("hwnd={0}, class={1}, caption={2}, unicode={3}, visible={4}, enabled={5}; ",
                window, name, Caption(window), IsWindowUnicode(window), IsWindowVisible(window), IsWindowEnabled(window));
            return true;
        };
        EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return result.ToString();
    }
    private static string ControlText(IntPtr window, uint timeout = 5000) {
        // Send WM_GETTEXT explicitly: caption helpers can return an empty
        // cached caption for another process's native Edit controls.
        StringBuilder text = new StringBuilder(1024);
        IntPtr copied;
        if (SendMessageTimeout(window,0xD,new IntPtr(text.Capacity),text,2,timeout,out copied) == IntPtr.Zero)
            throw new TimeoutException("Native control did not answer WM_GETTEXT");
        return text.ToString();
    }
    public static string DescribeChildren(IntPtr parent) {
        StringBuilder result = new StringBuilder();
        EnumCallback callback = (window, parameter) => {
            StringBuilder name = new StringBuilder(256);
            GetClassName(window, name, name.Capacity);
            Rect rect = Bounds(window);
            result.AppendFormat("hwnd={0}, id={1}, class={2}, text={3}, visible={4}, enabled={5}, rect={6},{7},{8},{9};\n",
                window, GetDlgCtrlID(window), name, ControlText(window), IsWindowVisible(window), IsWindowEnabled(window),
                rect.Left, rect.Top, rect.Right, rect.Bottom);
            return true;
        };
        EnumChildWindows(parent, callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return result.ToString();
    }
    public static Rect Bounds(IntPtr window) {
        Rect rect;
        if (!GetWindowRect(window, out rect)) throw new InvalidOperationException("GetWindowRect failed");
        return rect;
    }
    public static Rect ClientBounds(IntPtr window) {
        Rect rect;
        Point origin = new Point();
        if (!GetClientRect(window, out rect) || !ClientToScreen(window, ref origin)) throw new InvalidOperationException("Client bounds failed");
        return new Rect { Left=origin.X, Top=origin.Y, Right=origin.X+rect.Right, Bottom=origin.Y+rect.Bottom };
    }
    public static IntPtr Pack(int x, int y) { return new IntPtr(unchecked((int)((uint)(ushort)x | ((uint)(ushort)y << 16)))); }
    public static IntPtr Send(IntPtr window, uint message, IntPtr wparam, IntPtr lparam) {
        IntPtr result;
        if (SendMessageTimeout(window, message, wparam, lparam, 2, 5000, out result) == IntPtr.Zero)
            throw new TimeoutException("Watchman did not respond to Windows message " + message);
        return result;
    }
    public static void Click(IntPtr window, int x, int y, bool secondDoubleClick) {
        Send(window, 0x200, IntPtr.Zero, Pack(x,y));
        Send(window, secondDoubleClick ? 0x203u : 0x201u, new IntPtr(1), Pack(x,y));
        Send(window, 0x202, IntPtr.Zero, Pack(x,y));
    }
    // A posted click permits an ordinary modal ChooseColor dialog to open;
    // synchronous SendMessage would wait for that same dialog to be dismissed.
    public static void BeginClick(IntPtr window, int x, int y) {
        if (!PostMessage(window, 0x200, IntPtr.Zero, Pack(x,y)) ||
            !PostMessage(window, 0x201, new IntPtr(1), Pack(x,y)) ||
            !PostMessage(window, 0x202, IntPtr.Zero, Pack(x,y)))
            throw new InvalidOperationException("Could not open the native color picker");
    }
    private static IntPtr RgbField(IntPtr dialog, int id) {
        IntPtr edit = GetDlgItem(dialog,id);
        StringBuilder name = new StringBuilder(32);
        if (edit != IntPtr.Zero) GetClassName(edit,name,name.Capacity);
        if (edit == IntPtr.Zero || name.ToString() != "Edit" || !IsWindowEnabled(edit))
            throw new InvalidOperationException("Native RGB edit unavailable: " + id);
        return edit;
    }
    public static bool IsColorDialogReady(IntPtr dialog) {
        // EnumWindows can see #32770 before the dialog has created/initialized
        // its edits. Wait for visible, enabled controls and their real values,
        // rather than replacing that race with an arbitrary startup sleep.
        if (dialog == IntPtr.Zero || !IsWindowVisible(dialog) || !IsWindowEnabled(dialog)) return false;
        try {
            foreach (int id in new int[] {706,707,708}) {
                IntPtr edit = RgbField(dialog,id);
                byte value;
                if (!IsWindowVisible(edit) || !Byte.TryParse(ControlText(edit,100),out value)) return false;
            }
            return true;
        } catch (InvalidOperationException) { return false; }
        catch (TimeoutException) { return false; }
    }
    private static void InputKey(byte key) {
        keybd_event(key,0,0,UIntPtr.Zero); keybd_event(key,0,2,UIntPtr.Zero);
    }
    public static string ReadRgb(IntPtr dialog) {
        return String.Join(",",new string[] {
            ControlText(RgbField(dialog,706)), ControlText(RgbField(dialog,707)), ControlText(RgbField(dialog,708))
        });
    }
    public static string PrepareRgb(IntPtr dialog, byte red, byte green, byte blue) {
        // Exercise normal focus/keyboard notifications, not a registry write
        // or a synthetic OK result. Legacy Win32 Edit does not promise Ctrl+A;
        // Home + Shift+End selects its entire line even at the 3-digit limit.
        int[] ids = {706, 707, 708};
        byte[] values = {red, green, blue};
        SetForegroundWindow(dialog);
        for (int i=0; i<ids.Length; ++i) {
            Rect rect = Bounds(RgbField(dialog,ids[i]));
            ActualClick((rect.Left+rect.Right)/2,(rect.Top+rect.Bottom)/2,false);
            InputKey(36); // Home
            keybd_event(16,0,0,UIntPtr.Zero);
            InputKey(35); // Shift+End
            keybd_event(16,0,2,UIntPtr.Zero);
            foreach (char digit in values[i].ToString()) InputKey((byte)digit);
            Thread.Sleep(180);
        }
        InputKey(9); // Flush the final edit's focus-loss validation.
        Thread.Sleep(200);
        return ReadRgb(dialog);
    }
    public static void AcceptColor(IntPtr dialog) {
        IntPtr button = GetDlgItem(dialog,1);
        if (button == IntPtr.Zero) throw new InvalidOperationException("Native color OK button unavailable");
        Rect rect = Bounds(button);
        ActualClick((rect.Left+rect.Right)/2,(rect.Top+rect.Bottom)/2,false);
    }
    public static void Key(IntPtr window, int key) {
        Send(window, 0x100, new IntPtr(key), new IntPtr(1));
        Send(window, 0x101, new IntPtr(key), new IntPtr(unchecked((int)0xC0000001)));
    }
    public static void Wheel(IntPtr window, int delta) { Send(window, 0x20A, Pack(0, delta), IntPtr.Zero); }
    public static void ActualClick(int x, int y, bool doubleClick) {
        SetCursorPos(x,y); Thread.Sleep(70);
        mouse_event(2,0,0,0,UIntPtr.Zero); mouse_event(4,0,0,0,UIntPtr.Zero);
        if (doubleClick) { Thread.Sleep(80); mouse_event(2,0,0,0,UIntPtr.Zero); mouse_event(4,0,0,0,UIntPtr.Zero); }
    }
    public static void Drag(IntPtr window, int dx, int dy) {
        Rect rect = Bounds(window);
        int x = (rect.Left+rect.Right)/2, y=(rect.Top+rect.Bottom)/2;
        SetCursorPos(x,y); Thread.Sleep(80); mouse_event(2,0,0,0,UIntPtr.Zero);
        for (int step=1; step<=12; ++step) { SetCursorPos(x+dx*step/12,y+dy*step/12); Thread.Sleep(35); }
        mouse_event(4,0,0,0,UIntPtr.Zero); Thread.Sleep(250);
    }
    public static void EscapeMenu() { keybd_event(27,0,0,UIntPtr.Zero); Thread.Sleep(30); keybd_event(27,0,2,UIntPtr.Zero); }
    public static string MenuText(IntPtr menu, int index) {
        StringBuilder text = new StringBuilder(512);
        GetMenuString(menu,(uint)index,text,text.Capacity,0x400);
        return text.ToString();
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] private struct DisplayMode {
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)] public string DeviceName;
        public ushort SpecVersion, DriverVersion, Size, DriverExtra;
        public uint Fields;
        public int PositionX, PositionY;
        public uint Orientation, FixedOutput;
        public short Color, Duplex, YResolution, TTOption, Collate;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)] public string FormName;
        public ushort LogPixels;
        public uint BitsPerPel, Width, Height, Flags, Frequency, ICMMethod, ICMIntent,
            MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern bool EnumDisplaySettings(string device, int number, ref DisplayMode mode);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int ChangeDisplaySettings(ref DisplayMode mode, uint flags);
    private static DisplayMode savedDisplay;
    private static bool displayChanged;
    public static string WidenDisposableDesktop() {
        DisplayMode current = new DisplayMode();
        current.Size = (ushort)Marshal.SizeOf(typeof(DisplayMode));
        if (!EnumDisplaySettings(null,-1,ref current)) return "Could not enumerate current display";
        for (int index=0; index<512; ++index) {
            DisplayMode next = new DisplayMode(); next.Size = current.Size;
            if (!EnumDisplaySettings(null,index,ref next)) break;
            if (next.Width<1600 || next.Height<900 || next.Width>1920 || next.Height>1200 || next.BitsPerPel!=32) continue;
            savedDisplay = current;
            int result = ChangeDisplaySettings(ref next,0);
            displayChanged = result==0;
            return "Display change result="+result+", original="+current.Width+"x"+current.Height+", requested="+next.Width+"x"+next.Height;
        }
        return "No supported wider mode; original="+current.Width+"x"+current.Height;
    }
    public static void RestoreDisplay() { if (displayChanged) { ChangeDisplaySettings(ref savedDisplay,0); displayChanged=false; } }
    public static void BroadcastTheme() {
        IntPtr text=Marshal.StringToHGlobalUni("ImmersiveColorSet"), result;
        try { SendMessageTimeout(new IntPtr(0xFFFF),0x1A,IntPtr.Zero,text,2,1000,out result); }
        finally { Marshal.FreeHGlobal(text); }
    }
}
