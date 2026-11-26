using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using RomaineLife.Logline;
using Xunit;

namespace Logline.Tests;

// testdata/golden.json and testdata/redact.json, the cases every implementation runs, then
// the logger's own.
public class LogLineTests
{
    private static JsonElement Cases(string name) =>
        JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "testdata", name))).RootElement;

    private static Level LevelOf(string name) => name switch
    {
        "warn" => Level.Warn,
        "error" => Level.Error,
        _ => Level.Info,
    };

    [Fact]
    public void GoldenLinesFormatAndParse()
    {
        foreach (var c in Cases("golden.json").EnumerateArray())
        {
            var note = c.GetProperty("note").GetString();
            var time = DateTimeOffset.Parse(c.GetProperty("time").GetString()!, CultureInfo.InvariantCulture);
            var pid = c.GetProperty("pid").GetInt32();
            var level = LevelOf(c.GetProperty("level").GetString()!);
            var message = c.GetProperty("message").GetString()!;
            var input = c.TryGetProperty("message_in", out var m) ? m.GetString()! : message;
            var fields = c.GetProperty("fields").EnumerateArray()
                .Select(f => (Key: f[0].GetString()!, Value: (object?)f[1].GetString())).ToArray();
            var line = c.GetProperty("line").GetString()!;

            Assert.True(line == LogLine.Format(time, pid, level, input, fields), $"{note}:\n got {LogLine.Format(time, pid, level, input, fields)}\nwant {line}");
            var e = LogLine.Parse(line);
            Assert.Equal(time, e.Time);
            Assert.Equal(time.Offset, e.Time.Offset);
            Assert.Equal(pid, e.Pid);
            Assert.Equal(level, e.Level);
            Assert.Equal(message, e.Message);
            Assert.Equal(fields.Select(f => new Field(f.Key, (string)f.Value!)), e.Fields);
        }
    }

    [Theory]
    [InlineData("")]
    [InlineData("hello")]
    [InlineData("2026-11-25T10:30:43-08:00 pid=1 info no milliseconds")]
    [InlineData("2026-11-25 10:30:43.123 pid=1 info no zone")]
    [InlineData("2026/11/25 10:30:43 strip-owner's old format")]
    [InlineData("2026-11-25T10:30:43.123-08:00 info no pid")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=0 info zero pid")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=1 debug unknown level")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=1 info")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=1 info \"unterminated")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=1 info msg k=\"unterminated")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=1 info msg k=")]
    [InlineData("2026-11-25T10:30:43.123-08:00 pid=1 info \"quoted\"junk")]
    public void ParseRefusesWhatIsntTheStandard(string line)
    {
        Assert.False(LogLine.TryParse(line, out _, out var error));
        Assert.NotNull(error);
        Assert.Throws<FormatException>(() => LogLine.Parse(line));
    }

    [Fact]
    public void OddKeysStillMakeAStandardLine()
    {
        var line = LogLine.Format(DateTimeOffset.Now, 3, Level.Info, "x", ("Bad Key", 1), ("t", true));
        Assert.EndsWith(" info x bad_key=1 t=true", line);
        LogLine.Parse(line);
    }

    [Fact]
    public void RedactGolden()
    {
        foreach (var c in Cases("redact.json").EnumerateArray())
        {
            var note = c.GetProperty("note").GetString();
            var output = c.GetProperty("out").GetString()!;
            Assert.True(output == LogLine.Redact(c.GetProperty("in").GetString()!), $"{note}:\n got {LogLine.Redact(c.GetProperty("in").GetString()!)}\nwant {output}");
            Assert.True(output == LogLine.Redact(output), $"{note}: masking twice changed it");
        }
    }

    [Fact]
    public void AFileLogAppendsClosesAndRotates()
    {
        var dir = Directory.CreateTempSubdirectory("logline-").FullName;
        try
        {
            var path = Path.Combine(dir, "app", "app.log");
            var now = new DateTimeOffset(2026, 11, 25, 10, 0, 0, TimeSpan.FromHours(-8));
            var log = Logger.OpenFile(path);
            log.Now = () => now;
            log.Info("one", ("n", 1));
            log.Warn("two");
            var pid = Environment.ProcessId;
            Assert.Equal(LogLine.Format(now, pid, Level.Info, "one", ("n", 1)) + "\n" + LogLine.Format(now, pid, Level.Warn, "two") + "\n", File.ReadAllText(path));
            // Past the limit, the next line starts a new file and the old one moves aside.
            File.WriteAllText(path, new string('x', (int)LogLine.Limit + 1));
            log.Error("three");
            Assert.Equal(LogLine.Limit + 1, new FileInfo(path + ".old").Length);
            Assert.Equal(LogLine.Format(now, pid, Level.Error, "three") + "\n", File.ReadAllText(path));
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void OpenTakesTheOverridePath()
    {
        var path = Path.Combine(Path.GetTempPath(), "x.log");
        Environment.SetEnvironmentVariable("LOGLINE_TEST_LOG", path);
        try
        {
            Assert.Equal(path, Logger.Open("someapp", "LOGLINE_TEST_LOG").Path);
            Environment.SetEnvironmentVariable("LOGLINE_TEST_LOG", null);
            Assert.EndsWith(Path.Combine("someapp", "someapp.log"), Logger.Open("someapp", "LOGLINE_TEST_LOG").Path);
        }
        finally
        {
            Environment.SetEnvironmentVariable("LOGLINE_TEST_LOG", null);
        }
    }

    [Fact]
    public void AStreamLogWritesLines()
    {
        var writer = new StringWriter();
        var log = new Logger(writer);
        Assert.Null(log.Path);
        log.Info("served", ("status", 200));
        var text = writer.ToString();
        Assert.EndsWith(" info served status=200\n", text);
        LogLine.Parse(text[..^1]);
    }

    [Fact]
    public void ALoggerWritesTheLocalZone()
    {
        var writer = new StringWriter();
        new Logger(writer).Info("now");
        var time = LogLine.Parse(writer.ToString()[..^1]).Time;
        Assert.Equal(TimeZoneInfo.Local.GetUtcOffset(time), time.Offset);
    }
}
