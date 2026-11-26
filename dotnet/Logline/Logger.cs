using System;
using System.IO;
using System.Text;

namespace RomaineLife.Logline;

/// <summary>
/// A log file or stream. Its methods never throw: a line that can't be written is skipped,
/// since what failed is still on the program's screen or exit code. A file is opened and
/// closed for each line, so log-ship sees every line at once, and moved to &lt;name&gt;.old
/// past 1 MiB. Never log a secret: every home PC's everyday NAS sign-in can read the pile.
/// </summary>
public sealed class Logger
{
    private static readonly UTF8Encoding Utf8 = new(false);
    private readonly TextWriter? writer;
    private readonly object gate = new();

    private Logger(string? path, TextWriter? writer)
    {
        Path = path;
        this.writer = writer;
    }

    /// <summary>A log written to a stream: a service's stdout, which its platform ships.</summary>
    public Logger(TextWriter writer) : this(null, writer)
    {
    }

    /// <summary>The clock, which tests set.</summary>
    internal Func<DateTimeOffset> Now { get; set; } = () => DateTimeOffset.Now;

    /// <summary>The file the logger appends to, or null when it writes to a stream or has no path.</summary>
    public string? Path { get; }

    /// <summary>
    /// The log of the program named app: %LOCALAPPDATA%\&lt;app&gt;\&lt;app&gt;.log on Windows (the
    /// user's cache directory elsewhere, as Go's os.UserCacheDir), or the path in the
    /// environment variable overrideVariable when it is set, which tests and throwaway
    /// servers use to keep their lines out of the NAS's pile.
    /// </summary>
    public static Logger Open(string app, string overrideVariable)
    {
        var path = string.IsNullOrEmpty(overrideVariable) ? null : Environment.GetEnvironmentVariable(overrideVariable);
        if (string.IsNullOrEmpty(path))
        {
            var dir = CacheDir();
            path = dir == null ? null : System.IO.Path.Combine(dir, app, app + ".log");
        }
        return new Logger(path, null);
    }

    /// <summary>A log at path.</summary>
    public static Logger OpenFile(string path) => new(path, null);

    // Go's os.UserCacheDir: %LocalAppData%, ~/Library/Caches, or $XDG_CACHE_HOME (when
    // absolute) else ~/.cache; null when it can't be found.
    private static string? CacheDir()
    {
        static string? Get(string name) => Environment.GetEnvironmentVariable(name) is { Length: > 0 } v ? v : null;
        if (OperatingSystem.IsWindows())
        {
            return Get("LOCALAPPDATA");
        }
        if (OperatingSystem.IsMacOS())
        {
            return Get("HOME") is { } home ? System.IO.Path.Combine(home, "Library", "Caches") : null;
        }
        if (Get("XDG_CACHE_HOME") is { } xdg)
        {
            return System.IO.Path.IsPathRooted(xdg) ? xdg : null;
        }
        return Get("HOME") is { } h ? System.IO.Path.Combine(h, ".cache") : null;
    }

    /// <summary>Writes one info line; fields are (key, value) pairs.</summary>
    public void Info(string message, params (string Key, object? Value)[] fields) => Log(Level.Info, message, fields);

    /// <summary>Writes one warn line; fields are (key, value) pairs.</summary>
    public void Warn(string message, params (string Key, object? Value)[] fields) => Log(Level.Warn, message, fields);

    /// <summary>Writes one error line; fields are (key, value) pairs.</summary>
    public void Error(string message, params (string Key, object? Value)[] fields) => Log(Level.Error, message, fields);

    /// <summary>Writes one line at level.</summary>
    public void Log(Level level, string message, params (string Key, object? Value)[] fields)
    {
        try
        {
            var line = LogLine.Format(Now(), Environment.ProcessId, level, message, fields) + "\n";
            lock (gate)
            {
                if (writer != null)
                {
                    writer.Write(line);
                    writer.Flush();
                }
                else if (Path != null)
                {
                    Append(Path, line);
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException or ObjectDisposedException)
        {
        }
    }

    private static void Append(string path, string line)
    {
        var file = new FileInfo(path);
        if (file.Exists && file.Length > LogLine.Limit)
        {
            File.Move(path, path + ".old", true);
        }
        if (System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(path)) is { } dir)
        {
            if (OperatingSystem.IsWindows())
            {
                Directory.CreateDirectory(dir);
            }
            else
            {
                Directory.CreateDirectory(dir, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            }
        }
        var options = new FileStreamOptions
        {
            Mode = FileMode.Append,
            Access = FileAccess.Write,
            Share = FileShare.ReadWrite | FileShare.Delete,
        };
        if (!OperatingSystem.IsWindows())
        {
            options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        }
        using var stream = new FileStream(path, options);
        stream.Write(Utf8.GetBytes(line));
    }
}
