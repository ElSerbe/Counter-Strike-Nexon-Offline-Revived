// The launcher's look: the dark, teal-striped style of Counter-Strike Nexon's key art and the game's own dark menus.
// Everything is drawn here (no picture from the game ships with the launcher); ItsYuri's logo is an embedded resource.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace CsnzLauncher
{
    static class Theme
    {
        public static readonly Color Back = Color.FromArgb(10, 14, 15);
        public static readonly Color Panel = Color.FromArgb(19, 24, 26);
        public static readonly Color PanelLine = Color.FromArgb(38, 48, 50);
        public static readonly Color Field = Color.FromArgb(12, 16, 17);
        public static readonly Color Text = Color.FromArgb(232, 237, 237);
        public static readonly Color Muted = Color.FromArgb(132, 146, 146);
        public static readonly Color Accent = Color.FromArgb(32, 201, 151);       // the key art's teal
        public static readonly Color AccentDark = Color.FromArgb(14, 120, 92);
        public static readonly Color AccentText = Color.FromArgb(6, 22, 18);
        public static readonly Color Good = Color.FromArgb(64, 214, 160);
        public static readonly Color Warn = Color.FromArgb(240, 176, 64);
        public static readonly Color Bad = Color.FromArgb(232, 84, 84);
        // the game's menu buttons: dark grey with a lighter top edge
        public static readonly Color Button = Color.FromArgb(44, 50, 53);
        public static readonly Color ButtonHot = Color.FromArgb(58, 66, 70);
        public static readonly Color ButtonLine = Color.FromArgb(78, 88, 92);

        static FontFamily Family(params string[] names)
        {
            foreach (var n in names)
                try { return new FontFamily(n); } catch { }
            return FontFamily.GenericSansSerif;
        }
        public static readonly FontFamily Condensed = Family("Bahnschrift Condensed", "Bahnschrift", "Impact", "Segoe UI");
        public static readonly FontFamily Ui = Family("Bahnschrift", "Segoe UI");
        public static Font Display(float px, FontStyle style = FontStyle.Bold) => new Font(Condensed, px, style, GraphicsUnit.Pixel);
        public static Font UiFont(float px, FontStyle style = FontStyle.Regular) => new Font(Ui, px, style, GraphicsUnit.Pixel);

        public static Image Logo()
        {
            using (var s = typeof(Theme).Assembly.GetManifestResourceStream("CsnzLauncher.logo.png"))
                return s == null ? null : Image.FromStream(new MemoryStream(ReadAll(s)));
        }
        static byte[] ReadAll(Stream s)
        {
            using (var m = new MemoryStream())
            {
                s.CopyTo(m);
                return m.ToArray();
            }
        }

        public static GraphicsPath Chamfer(Rectangle r, int cut)
        {
            // corners cut at top-left and bottom-right, like the game's HUD panels
            var p = new GraphicsPath();
            p.AddPolygon(new[] { new Point(r.Left + cut, r.Top), new Point(r.Right, r.Top), new Point(r.Right, r.Bottom - cut),
                                 new Point(r.Right - cut, r.Bottom), new Point(r.Left, r.Bottom), new Point(r.Left, r.Top + cut) });
            return p;
        }

        // dark controls of the standard kind (text boxes, the log)
        public static void Dark(Control c)
        {
            c.BackColor = Field;
            c.ForeColor = Text;
            if (c is TextBox t)
                t.BorderStyle = BorderStyle.FixedSingle;
        }

        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
        // a borderless window moves when its header is dragged
        public static void DragWindow(Form f)
        {
            ReleaseCapture();
            SendMessage(f.Handle, 0xA1, (IntPtr)2, IntPtr.Zero); // WM_NCLBUTTONDOWN, HTCAPTION
        }
    }

    // a button drawn like the game's menus; Primary is the big teal one
    class FlatButton : Button
    {
        public bool Primary;
        public string Sub; // a second, smaller line
        bool hot, down;

        public FlatButton()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            FlatStyle = FlatStyle.Flat;
            Cursor = Cursors.Hand;
            Font = Theme.Display(17);
            ForeColor = Theme.Text;
        }

        protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hot = down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.Clear(Parent?.BackColor ?? Theme.Back);
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            int cut = Primary ? 14 : 7;
            using (var path = Theme.Chamfer(r, cut))
            {
                Color top, bottom, line, text;
                if (!Enabled)
                {
                    top = bottom = Color.FromArgb(28, 33, 35);
                    line = Theme.PanelLine;
                    text = Color.FromArgb(90, 100, 100);
                }
                else if (Primary)
                {
                    top = hot ? Color.FromArgb(70, 228, 180) : Theme.Accent;
                    bottom = down ? Theme.AccentDark : Color.FromArgb(18, 160, 118);
                    line = Color.FromArgb(120, 240, 200);
                    text = Theme.AccentText;
                }
                else
                {
                    top = hot ? Theme.ButtonHot : Theme.Button;
                    bottom = down ? Color.FromArgb(24, 28, 30) : Color.FromArgb(30, 35, 37);
                    line = hot ? Theme.Accent : Theme.ButtonLine;
                    text = Theme.Text;
                }
                using (var b = new LinearGradientBrush(r, top, bottom, LinearGradientMode.Vertical))
                    g.FillPath(b, path);
                using (var p = new Pen(line))
                    g.DrawPath(p, path);
                // the light top edge of the game's buttons
                if (Enabled && !Primary)
                    using (var p = new Pen(Color.FromArgb(40, 255, 255, 255)))
                        g.DrawLine(p, cut + 1, 1, Width - 2, 1);

                var sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
                using (var tb = new SolidBrush(text))
                {
                    if (string.IsNullOrEmpty(Sub))
                        g.DrawString(Text.ToUpperInvariant(), Font, tb, r, sf);
                    else
                    {
                        var main = new Rectangle(0, 0, Width, Height * 2 / 3);
                        var sub = new Rectangle(0, Height / 2 + 4, Width, Height / 2 - 6);
                        g.DrawString(Text.ToUpperInvariant(), Font, tb, main, sf);
                        using (var f = Theme.UiFont(12))
                        using (var sb = new SolidBrush(Color.FromArgb(Primary ? 170 : 150, text)))
                            g.DrawString(Sub, f, sb, sub, sf);
                    }
                }
            }
        }
    }

    // the More menu in the dark theme
    class DarkMenuRenderer : ToolStripProfessionalRenderer
    {
        public DarkMenuRenderer() : base(new DarkColors()) { }
        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            e.TextColor = e.Item.Enabled ? Theme.Text : Theme.Muted;
            base.OnRenderItemText(e);
        }
        class DarkColors : ProfessionalColorTable
        {
            public override Color ToolStripDropDownBackground => Theme.Panel;
            public override Color MenuBorder => Theme.PanelLine;
            public override Color MenuItemBorder => Theme.Accent;
            public override Color MenuItemSelected => Color.FromArgb(28, 70, 58);
            public override Color MenuItemSelectedGradientBegin => Color.FromArgb(28, 70, 58);
            public override Color MenuItemSelectedGradientEnd => Color.FromArgb(28, 70, 58);
            public override Color SeparatorDark => Theme.PanelLine;
            public override Color SeparatorLight => Theme.PanelLine;
            public override Color ImageMarginGradientBegin => Theme.Panel;
            public override Color ImageMarginGradientMiddle => Theme.Panel;
            public override Color ImageMarginGradientEnd => Theme.Panel;
        }
    }

    // an on/off switch with its label
    class Toggle : CheckBox
    {
        public Toggle()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            Cursor = Cursors.Hand;
            Font = Theme.UiFont(13);
            ForeColor = Theme.Text;
            AutoSize = false;
            Height = 26;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.Clear(Parent?.BackColor ?? Theme.Panel);
            var track = new Rectangle(0, 4, 40, 18);
            using (var path = new GraphicsPath())
            {
                path.AddArc(track.Left, track.Top, track.Height, track.Height, 90, 180);
                path.AddArc(track.Right - track.Height, track.Top, track.Height, track.Height, 270, 180);
                path.CloseFigure();
                using (var b = new SolidBrush(Checked ? Theme.Accent : Color.FromArgb(56, 64, 68)))
                    g.FillPath(b, path);
            }
            int x = Checked ? track.Right - track.Height + 2 : track.Left + 2;
            using (var b = new SolidBrush(Checked ? Color.White : Color.FromArgb(170, 180, 180)))
                g.FillEllipse(b, x, track.Top + 2, track.Height - 4, track.Height - 4);
            using (var tb = new SolidBrush(ForeColor))
                g.DrawString(Text, Font, tb, new RectangleF(50, 0, Width - 50, Height), new StringFormat { LineAlignment = StringAlignment.Center, Trimming = StringTrimming.EllipsisCharacter });
        }
    }

    // a dark panel with a thin border and a small teal title
    class Card : Panel
    {
        public string Title;
        public Card()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            BackColor = Theme.Panel;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Parent?.BackColor ?? Theme.Back);
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            using (var path = Theme.Chamfer(r, 10))
            {
                using (var b = new SolidBrush(Theme.Panel))
                    g.FillPath(b, path);
                using (var p = new Pen(Theme.PanelLine))
                    g.DrawPath(p, path);
            }
            if (!string.IsNullOrEmpty(Title))
            {
                g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                using (var b = new SolidBrush(Theme.Accent))
                    g.FillRectangle(b, 16, 16, 3, 14);
                using (var f = Theme.Display(15))
                using (var b = new SolidBrush(Theme.Text))
                    g.DrawString(Title.ToUpperInvariant(), f, b, 24, 13);
            }
        }
    }

    // the header: the key art's teal brush stripes, the big title, ItsYuri's logo and the window buttons
    class Header : Control
    {
        readonly Image logo = Theme.Logo();
        public string Credits = "";
        Rectangle closeBox, minBox, logoBox, patreonText;
        const string PatreonLine = "Enjoying it? Support the development on Patreon ♥";
        int hot = -1; // 0 minimize, 1 close, 2 music, 3 Patreon, 4 Discord
        public event EventHandler LogoClicked, MusicClicked, PatreonClicked, DiscordClicked;
        bool musicOn;
        public bool MusicOn { get => musicOn; set { musicOn = value; Invalidate(); } }
        readonly ToolTip tip = new ToolTip();
        static readonly string[] IconTips = { null, null, "Music on / off", "Support the project on Patreon", "Join the Discord" };

        public Header()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            BackColor = Theme.Back;
        }

        // the three square buttons left of the window buttons: music, Patreon, Discord
        Rectangle IconBox(int i) => new Rectangle(Width - 92 - 14 - (3 - i) * 42 - 4, 8, 38, 34);

        int HitTest(Point p)
        {
            if (closeBox.Contains(p)) return 1;
            if (minBox.Contains(p)) return 0;
            for (int i = 0; i < 3; i++)
                if (IconBox(i).Contains(p)) return 2 + i;
            if (patreonText.Contains(p)) return 3;
            return -1;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            int h = HitTest(e.Location);
            Cursor = h >= 0 || logoBox.Contains(e.Location) ? Cursors.Hand : Cursors.Default;
            if (h != hot)
            {
                hot = h;
                Invalidate();
                if (h >= 2 && IconBox(h - 2).Contains(e.Location)) tip.Show(IconTips[h], this, IconBox(h - 2).Left, IconBox(h - 2).Bottom + 6, 2500);
                else tip.Hide(this);
            }
            base.OnMouseMove(e);
        }
        protected override void OnMouseLeave(EventArgs e) { hot = -1; tip.Hide(this); Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e)
        {
            var f = FindForm();
            if (e.Button != MouseButtons.Left || f == null)
                return;
            switch (HitTest(e.Location))
            {
                case 1: f.Close(); break;
                case 0: f.WindowState = FormWindowState.Minimized; break;
                case 2: MusicClicked?.Invoke(this, EventArgs.Empty); break;
                case 3: PatreonClicked?.Invoke(this, EventArgs.Empty); break;
                case 4: DiscordClicked?.Invoke(this, EventArgs.Empty); break;
                default:
                    if (logoBox.Contains(e.Location)) LogoClicked?.Invoke(this, EventArgs.Empty);
                    else Theme.DragWindow(f);
                    break;
            }
        }

        // a chamfered dark square like the launcher's buttons, with a teal glyph drawn in a 20x20 box
        void IconButton(Graphics g, int i, Action<Graphics, Brush> glyph)
        {
            var r = IconBox(i);
            bool isHot = hot == 2 + i;
            using (var path = Theme.Chamfer(r, 6))
            {
                using (var b = new LinearGradientBrush(r, Color.FromArgb(isHot ? 235 : 210, isHot ? Theme.ButtonHot : Theme.Button),
                                                          Color.FromArgb(isHot ? 235 : 210, 20, 25, 27), LinearGradientMode.Vertical))
                    g.FillPath(b, path);
                using (var p = new Pen(isHot ? Theme.Accent : Color.FromArgb(150, Theme.ButtonLine)))
                    g.DrawPath(p, path);
            }
            var state = g.Save();
            g.TranslateTransform(r.X + (r.Width - 20) / 2f, r.Y + (r.Height - 20) / 2f);
            using (var b = new SolidBrush(isHot ? Color.FromArgb(120, 240, 200) : Theme.Accent))
                glyph(g, b);
            g.Restore(state);
        }

        // a speaker, with sound waves when the music plays and a cross when it doesn't
        void SpeakerGlyph(Graphics g, Brush b)
        {
            g.FillPolygon(b, new[] { new PointF(2, 7), new PointF(6, 7), new PointF(11, 2.5f), new PointF(11, 17.5f), new PointF(6, 13), new PointF(2, 13) });
            using (var p = new Pen(b, 1.8f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
            {
                if (musicOn)
                {
                    g.DrawArc(p, 9.5f, 6, 6, 8, -55, 110);
                    g.DrawArc(p, 9, 2.5f, 10, 15, -55, 110);
                }
                else
                {
                    g.DrawLine(p, 13.5f, 7.5f, 18.5f, 12.5f);
                    g.DrawLine(p, 18.5f, 7.5f, 13.5f, 12.5f);
                }
            }
        }

        // Patreon's mark: a round disc and a tall bar on its left
        static void PatreonGlyph(Graphics g, Brush b)
        {
            g.FillRectangle(b, 2, 2.5f, 3.6f, 15);
            g.FillEllipse(b, 7.2f, 2.5f, 11, 11);
        }

        // Discord's mark: the controller-shaped face with two eyes cut out
        static void DiscordGlyph(Graphics g, Brush b)
        {
            using (var path = new GraphicsPath(FillMode.Alternate))
            {
                path.AddBezier(5.2f, 3.6f, 8, 2.6f, 12, 2.6f, 14.8f, 3.6f);
                path.AddBezier(14.8f, 3.6f, 17.6f, 7.5f, 19.4f, 11.5f, 19.2f, 15.4f);
                path.AddBezier(19.2f, 15.4f, 17.8f, 16.6f, 16.2f, 17.3f, 14.6f, 17.6f);
                path.AddBezier(14.6f, 17.6f, 14.1f, 16.9f, 13.7f, 16.3f, 13.4f, 15.6f);
                path.AddBezier(13.4f, 15.6f, 11.2f, 16.3f, 8.8f, 16.3f, 6.6f, 15.6f);
                path.AddBezier(6.6f, 15.6f, 6.3f, 16.3f, 5.9f, 16.9f, 5.4f, 17.6f);
                path.AddBezier(5.4f, 17.6f, 3.8f, 17.3f, 2.2f, 16.6f, 0.8f, 15.4f);
                path.AddBezier(0.8f, 15.4f, 0.6f, 11.5f, 2.4f, 7.5f, 5.2f, 3.6f);
                path.CloseFigure();
                path.AddEllipse(5.4f, 8.6f, 3.4f, 3.8f);
                path.AddEllipse(11.2f, 8.6f, 3.4f, 3.8f);
                g.FillPath(b, path);
            }
        }

        // a diagonal brush stroke with ragged edges (fixed seed: the same every time)
        static void Stroke(Graphics g, Brush b, float x0, float y0, float x1, float y1, float width, int seed)
        {
            var rnd = new Random(seed);
            float dx = x1 - x0, dy = y1 - y0, len = (float)Math.Sqrt(dx * dx + dy * dy);
            float nx = -dy / len, ny = dx / len;
            int steps = 40;
            var pts = new PointF[steps * 2 + 2];
            for (int i = 0; i <= steps; i++)
            {
                float t = i / (float)steps;
                float px = x0 + dx * t, py = y0 + dy * t;
                float w1 = width / 2 * (0.82f + (float)rnd.NextDouble() * 0.3f);
                float w2 = width / 2 * (0.82f + (float)rnd.NextDouble() * 0.3f);
                pts[i] = new PointF(px + nx * w1, py + ny * w1);
                pts[steps * 2 + 1 - i] = new PointF(px - nx * w2, py - ny * w2);
            }
            g.FillPolygon(b, pts);
            // a few loose bristle marks along the edges
            for (int i = 0; i < 26; i++)
            {
                float t = (float)rnd.NextDouble();
                float side = rnd.Next(2) == 0 ? 1 : -1;
                float off = width / 2 * (1.0f + (float)rnd.NextDouble() * 0.35f) * side;
                float px = x0 + dx * t + nx * off, py = y0 + dy * t + ny * off;
                float l = 18 + (float)rnd.NextDouble() * 60;
                using (var p = new Pen(b, 1.5f + (float)rnd.NextDouble() * 3))
                    g.DrawLine(p, px, py, px + dx / len * l, py + dy / len * l);
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            int w = Width, h = Height;

            using (var bg = new LinearGradientBrush(new Rectangle(0, 0, w, h), Color.FromArgb(14, 22, 22), Color.FromArgb(6, 10, 11), 90f))
                g.FillRectangle(bg, 0, 0, w, h);

            // big outlined words behind everything, as in the key art
            using (var f = Theme.Display(118))
            using (var pen = new Pen(Color.FromArgb(26, 120, 200, 180), 1.5f))
            {
                for (int row = 0; row < 3; row++)
                {
                    using (var path = new GraphicsPath())
                    {
                        path.AddString("COUNTER-STRIKE  COUNTER-STRIKE", Theme.Condensed, (int)FontStyle.Bold, 118, new PointF(-60 + row * 40, -26 + row * 92), StringFormat.GenericDefault);
                        g.DrawPath(pen, path);
                    }
                }
            }

            // the teal brush strokes, top right to bottom left
            using (var b1 = new SolidBrush(Color.FromArgb(225, 24, 168, 128)))
            using (var b2 = new SolidBrush(Color.FromArgb(200, 32, 201, 151)))
            using (var b3 = new SolidBrush(Color.FromArgb(70, 32, 201, 151)))
            {
                Stroke(g, b3, w * 0.30f, h + 40, w * 0.95f, -60, 150, 7);
                Stroke(g, b1, w * 0.44f, h + 30, w * 1.02f, -20, 92, 3);
                Stroke(g, b2, w * 0.66f, h + 20, w * 1.10f, 40, 54, 11);
            }

            // shade so the title stays readable, and a fade into the window below
            using (var shade = new LinearGradientBrush(new Rectangle(0, 0, w, h), Color.FromArgb(230, 8, 12, 13), Color.FromArgb(0, 8, 12, 13), 0f))
            {
                var blend = new ColorBlend { Colors = new[] { Color.FromArgb(235, 8, 12, 13), Color.FromArgb(120, 8, 12, 13), Color.FromArgb(0, 8, 12, 13) }, Positions = new[] { 0f, 0.4f, 1f } };
                shade.InterpolationColors = blend;
                g.FillRectangle(shade, 0, 0, w, h);
            }
            using (var fade = new LinearGradientBrush(new Rectangle(0, h - 60, w, 60), Color.FromArgb(0, Theme.Back), Theme.Back, 90f))
                g.FillRectangle(fade, 0, h - 60, w, 61);

            // the title
            using (var f = Theme.Display(84))
            using (var b = new SolidBrush(Color.FromArgb(236, 240, 240)))
                g.DrawString("COUNTER-STRIKE", f, b, 26, 44);
            using (var f = Theme.Display(26))
            using (var b = new SolidBrush(Theme.Accent))
                g.DrawString("NEXON: OFFLINE REVIVED", f, b, 32, 134);
            using (var f = Theme.UiFont(12))
            using (var b = new SolidBrush(Color.FromArgb(150, 170, 170)))
                g.DrawString(Credits, f, b, 33, 170);

            // ItsYuri's logo, with a teal ring
            int size = 104;
            logoBox = new Rectangle(w - size - 40, 56, size, size);
            if (logo != null)
            {
                using (var ring = new SolidBrush(Theme.Accent))
                    g.FillEllipse(ring, logoBox.X - 4, logoBox.Y - 4, size + 8, size + 8);
                using (var clip = new GraphicsPath())
                {
                    clip.AddEllipse(logoBox);
                    var old = g.Clip;
                    g.SetClip(clip);
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.DrawImage(logo, logoBox);
                    g.Clip = old;
                }
                using (var f = Theme.Display(14))
                using (var b = new SolidBrush(Theme.Text))
                {
                    var sf = new StringFormat { Alignment = StringAlignment.Center };
                    g.DrawString("BY ITSYURI", f, b, new RectangleF(logoBox.X - 20, logoBox.Bottom + 8, size + 40, 20), sf);
                }
            }

            // window buttons
            minBox = new Rectangle(w - 92, 0, 46, 32);
            closeBox = new Rectangle(w - 46, 0, 46, 32);
            if (hot == 0) using (var b = new SolidBrush(Color.FromArgb(50, 255, 255, 255))) g.FillRectangle(b, minBox);
            if (hot == 1) using (var b = new SolidBrush(Color.FromArgb(200, 196, 43, 28))) g.FillRectangle(b, closeBox);
            using (var p = new Pen(Theme.Text, 1.2f))
            {
                g.DrawLine(p, minBox.X + 18, 16, minBox.X + 28, 16);
                g.DrawLine(p, closeBox.X + 18, 11, closeBox.X + 28, 21);
                g.DrawLine(p, closeBox.X + 28, 11, closeBox.X + 18, 21);
            }

            IconButton(g, 0, SpeakerGlyph);
            IconButton(g, 1, PatreonGlyph);
            IconButton(g, 2, DiscordGlyph);

            // a small line under the icons asking for support, clickable like the Patreon button
            using (var f = Theme.UiFont(12, hot == 3 ? FontStyle.Underline : FontStyle.Regular))
            {
                var ts = g.MeasureString(PatreonLine, f);
                int right = IconBox(2).Right - 14;
                patreonText = new Rectangle(right - (int)ts.Width, IconBox(0).Bottom + 4, (int)ts.Width + 2, (int)ts.Height);
                using (var b = new SolidBrush(hot == 3 ? Color.FromArgb(120, 240, 200) : Color.FromArgb(225, 200, 236, 226)))
                    g.DrawString(PatreonLine, f, b, patreonText.Location);
            }
        }
    }
}
