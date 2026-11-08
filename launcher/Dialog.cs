// The launcher's message boxes, drawn like its main window (dark panel, cut corners, teal title, flat buttons)
// instead of Windows' grey MessageBox. Same arguments and results as MessageBox.Show.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Windows.Forms;

namespace CsnzLauncher
{
    class DarkBox : Form
    {
        const int Pad = 22, TextWidth = 440;
        readonly string title;
        readonly Color stripe;

        DarkBox(string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon, string[] names = null)
        {
            title = string.IsNullOrEmpty(caption) ? "Counter-Strike Nexon: Offline Revived" : caption;
            stripe = icon == MessageBoxIcon.Warning ? Theme.Warn : icon == MessageBoxIcon.Error ? Theme.Bad : Theme.Accent;
            Text = title;
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            KeyPreview = true;
            Font = Theme.UiFont(14);
            BackColor = Theme.Panel; // the buttons and the text clear to it
            ForeColor = Theme.Text;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.ResizeRedraw, true);

            var body = new Label { Text = text, AutoSize = false, ForeColor = Theme.Text, BackColor = Theme.Panel, UseMnemonic = false };
            int h = TextRenderer.MeasureText(text, Font, new Size(TextWidth, int.MaxValue), TextFormatFlags.WordBreak).Height + 6;
            body.SetBounds(Pad, 58, TextWidth, h);

            string[] labels;
            DialogResult[] results;
            switch (buttons)
            {
                case MessageBoxButtons.YesNo: labels = new[] { "Yes", "No" }; results = new[] { DialogResult.Yes, DialogResult.No }; break;
                case MessageBoxButtons.YesNoCancel: labels = new[] { "Yes", "No", "Cancel" }; results = new[] { DialogResult.Yes, DialogResult.No, DialogResult.Cancel }; break;
                case MessageBoxButtons.OKCancel: labels = new[] { "OK", "Cancel" }; results = new[] { DialogResult.OK, DialogResult.Cancel }; break;
                default: labels = new[] { "OK" }; results = new[] { DialogResult.OK }; break;
            }
            // other words on the buttons ("Extract now" for Yes), same results
            if (names != null)
                for (int i = 0; i < labels.Length && i < names.Length; i++)
                    if (!string.IsNullOrEmpty(names[i]))
                        labels[i] = names[i];
            ClientSize = new Size(TextWidth + 2 * Pad, body.Bottom + 22 + 36 + Pad);
            int x = ClientSize.Width - Pad;
            for (int i = labels.Length - 1; i >= 0; i--)
            {
                var b = new FlatButton { Text = labels[i], DialogResult = results[i], Primary = i == 0 };
                b.Size = new Size(Math.Max(i == 0 ? 110 : 96, TextRenderer.MeasureText(labels[i].ToUpperInvariant(), b.Font).Width + 32), 36);
                x -= b.Width;
                b.Location = new Point(x, ClientSize.Height - Pad - b.Height);
                x -= 10;
                Controls.Add(b);
                if (i == 0) AcceptButton = b;
                if (results[i] == DialogResult.Cancel || (results[i] == DialogResult.No && buttons == MessageBoxButtons.YesNo) || labels.Length == 1)
                    CancelButton = b;
            }
            Controls.Add(body);
            body.MouseDown += (s, e) => { if (e.Button == MouseButtons.Left) Theme.DragWindow(this); };
            Shown += (s, e) => (AcceptButton as Control)?.Focus();
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
                Theme.DragWindow(this);
            base.OnMouseDown(e);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.Clear(Theme.Back);
            var r = new Rectangle(0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
            using (var path = Theme.Chamfer(r, 16))
            {
                using (var b = new SolidBrush(Theme.Panel))
                    g.FillPath(b, path);
                using (var p = new Pen(Theme.ButtonLine))
                    g.DrawPath(p, path);
            }
            using (var b = new SolidBrush(stripe))
                g.FillRectangle(b, Pad, 22, 4, 20);
            using (var f = Theme.Display(20))
            using (var b = new SolidBrush(Theme.Text))
                g.DrawString(title.ToUpperInvariant(), f, b, Pad + 12, 18);
        }

        // a borderless window gets no shadow of its own: ask Windows for the drop shadow
        protected override CreateParams CreateParams
        {
            get { var cp = base.CreateParams; cp.ClassStyle |= 0x20000; return cp; } // CS_DROPSHADOW
        }

        public static DialogResult Show(IWin32Window owner, string text, string caption = null,
            MessageBoxButtons buttons = MessageBoxButtons.OK, MessageBoxIcon icon = MessageBoxIcon.None, string[] names = null)
        {
            using (var d = new DarkBox(text, caption, buttons, icon, names))
            {
                d.StartPosition = owner != null ? FormStartPosition.CenterParent : FormStartPosition.CenterScreen;
                if (owner == null)
                    d.ShowInTaskbar = true;
                return owner != null ? d.ShowDialog(owner) : d.ShowDialog();
            }
        }

        // draws the box into a .png without showing it (to check how it looks)
        public static void Render(string png, string text, string caption, MessageBoxButtons buttons, MessageBoxIcon icon, string[] names = null)
        {
            using (var d = new DarkBox(text, caption, buttons, icon, names))
            using (var bmp = new Bitmap(d.Width, d.Height))
            {
                // shown off screen so the text and the buttons get their windows, then copied
                d.StartPosition = FormStartPosition.Manual;
                d.Location = new Point(-5000, -5000);
                d.Show();
                Application.DoEvents();
                d.DrawToBitmap(bmp, new Rectangle(Point.Empty, d.Size));
                d.Close();
                bmp.Save(png);
            }
        }

        public static DialogResult Show(string text, string caption = null,
            MessageBoxButtons buttons = MessageBoxButtons.OK, MessageBoxIcon icon = MessageBoxIcon.None, string[] names = null) =>
            Show(null, text, caption, buttons, icon, names);
    }
}
