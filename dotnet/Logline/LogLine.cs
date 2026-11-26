// The home estate's one log line (at-home-tools docs/app-logs.md, AHT-204), written and
// read exactly as the Go package at the repository's root does; both run
// testdata/golden.json and testdata/redact.json:
//
//   2026-11-25T10:30:43.123-08:00 pid=47672 warn download stalled step=tools took=41.2s
//
// The time (RFC 3339, milliseconds, the local zone's offset), the process id, the level
// (info, warn or error), the message, then key=value fields. A message holding '=',
// starting with a quote, or empty is written quoted, so the first word with '=' always
// starts the fields; a value holding a space, quote, '=' or control character is quoted,
// with Go's escapes.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace RomaineLife.Logline;

/// <summary>How much a line matters.</summary>
public enum Level
{
    /// <summary>info</summary>
    Info,
    /// <summary>warn</summary>
    Warn,
    /// <summary>error</summary>
    Error,
}

/// <summary>One key=value of a parsed line.</summary>
public readonly record struct Field(string Key, string Value);

/// <summary>A parsed line.</summary>
public sealed record Entry(DateTimeOffset Time, int Pid, Level Level, string Message, IReadOnlyList<Field> Fields);

/// <summary>Writes and reads the line: <see cref="Format"/>, <see cref="Parse"/>, <see cref="Redact"/>.</summary>
public static class LogLine
{
    /// <summary>The size past which a log file moves to &lt;name&gt;.old and starts again.</summary>
    public const long Limit = 1 << 20;

    /// <summary>What <see cref="Redact"/> writes in place of a credential.</summary>
    public const string Marker = "***";

    /// <summary>"info", "warn" or "error".</summary>
    public static string Name(this Level level) => level switch
    {
        Level.Warn => "warn",
        Level.Error => "error",
        _ => "info",
    };

    /// <summary>The line's time: RFC 3339 with milliseconds and the zone's offset, UTC written Z.</summary>
    public static string FormatTime(DateTimeOffset time)
    {
        var stamp = time.ToString("yyyy'-'MM'-'dd'T'HH':'mm':'ss'.'fff", CultureInfo.InvariantCulture);
        var offset = (int)time.Offset.TotalMinutes;
        if (offset == 0)
        {
            return stamp + "Z";
        }
        var abs = Math.Abs(offset);
        return string.Create(CultureInfo.InvariantCulture, $"{stamp}{(offset < 0 ? '-' : '+')}{abs / 60:00}:{abs % 60:00}");
    }

    /// <summary>
    /// One line, without its newline. Keys are lower-case words ([a-z][a-z0-9_.-]*), another is
    /// written bad_key; a value is written with the invariant culture.
    /// </summary>
    public static string Format(DateTimeOffset time, int pid, Level level, string? message, params (string Key, object? Value)[] fields)
    {
        var b = new StringBuilder();
        b.Append(FormatTime(time)).Append(" pid=").Append(pid.ToString(CultureInfo.InvariantCulture)).Append(' ').Append(level.Name()).Append(' ');
        var msg = OneLine(message ?? "");
        if (msg.Length == 0 || msg.Contains('=') || msg.StartsWith('"'))
        {
            Quote(b, msg);
        }
        else
        {
            b.Append(msg);
        }
        foreach (var (key, value) in fields)
        {
            b.Append(' ').Append(ValidKey(key) ? key : "bad_key").Append('=');
            FormatValue(b, Text(value));
        }
        return b.ToString();
    }

    private static string Text(object? value) => value switch
    {
        null => "null",
        bool v => v ? "true" : "false",
        IFormattable f => f.ToString(null, CultureInfo.InvariantCulture),
        _ => value.ToString() ?? "",
    };

    private static bool ValidKey(string? key)
    {
        if (string.IsNullOrEmpty(key) || key[0] < 'a' || key[0] > 'z')
        {
            return false;
        }
        foreach (var c in key)
        {
            if (!(c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_' or '.' or '-'))
            {
                return false;
            }
        }
        return true;
    }

    // Go's unicode.IsControl: C0, DEL and C1.
    private static bool IsControl(int code) => code < 0x20 || code is >= 0x7f and <= 0x9f;

    // Go's unicode.IsSpace.
    private static bool IsSpace(int code) =>
        code is >= 0x09 and <= 0x0d or 0x20 or 0x85 or 0xa0 or 0x1680 or >= 0x2000 and <= 0x200a
            or 0x2028 or 0x2029 or 0x202f or 0x205f or 0x3000;

    // Go's strconv.Quote for the text a line holds: \" \\ and Go's short escapes, other C0
    // characters and DEL as \x.., C1 as \u..... A lone surrogate is written U+FFFD.
    private static void Quote(StringBuilder b, string text)
    {
        b.Append('"');
        foreach (var rune in text.EnumerateRunes())
        {
            var code = rune.Value;
            switch (code)
            {
                case '"': b.Append("\\\""); break;
                case '\\': b.Append("\\\\"); break;
                case '\n': b.Append("\\n"); break;
                case '\r': b.Append("\\r"); break;
                case '\t': b.Append("\\t"); break;
                case 0x07: b.Append("\\a"); break;
                case 0x08: b.Append("\\b"); break;
                case 0x0c: b.Append("\\f"); break;
                case 0x0b: b.Append("\\v"); break;
                default:
                    if (code < 0x20 || code == 0x7f)
                    {
                        b.Append("\\x").Append(code.ToString("x2", CultureInfo.InvariantCulture));
                    }
                    else if (IsControl(code))
                    {
                        b.Append("\\u").Append(code.ToString("x4", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        b.Append(rune.ToString());
                    }
                    break;
            }
        }
        b.Append('"');
    }

    private static void FormatValue(StringBuilder b, string value)
    {
        // A lone surrogate is C#'s text that isn't UTF-8, which Go quotes.
        var quote = value.Length == 0;
        for (var i = 0; i < value.Length && !quote; i++)
        {
            var c = value[i];
            if (char.IsHighSurrogate(c) && i + 1 < value.Length && char.IsLowSurrogate(value[i + 1]))
            {
                i++;
                continue;
            }
            quote = c is ' ' or '"' or '=' || IsControl(c) || char.IsSurrogate(c);
        }
        if (quote)
        {
            Quote(b, value);
        }
        else
        {
            b.Append(value);
        }
    }

    // Keeps a message on its line: line breaks become " | ", tabs spaces, other control
    // characters go, and a lone surrogate becomes U+FFFD.
    private static string OneLine(string text)
    {
        text = text.Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n').Trim('\n').Replace("\n", " | ", StringComparison.Ordinal);
        var runes = new List<Rune>();
        foreach (var rune in text.EnumerateRunes())
        {
            if (rune.Value == '\t')
            {
                runes.Add(new Rune(' '));
            }
            else if (!IsControl(rune.Value))
            {
                runes.Add(rune);
            }
        }
        int start = 0, end = runes.Count;
        while (start < end && IsSpace(runes[start].Value))
        {
            start++;
        }
        while (end > start && IsSpace(runes[end - 1].Value))
        {
            end--;
        }
        var b = new StringBuilder();
        for (var i = start; i < end; i++)
        {
            b.Append(runes[i].ToString());
        }
        return b.ToString();
    }

    private static readonly Regex Stamp = new(@"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})\.(\d{3})(?:(Z)|([+-])(\d{2}):(\d{2}))$", RegexOptions.CultureInvariant);

    // The line's time: exactly three fractional digits and a zone, Z or +hh:mm.
    private static DateTimeOffset? ParseTime(string stamp)
    {
        var m = Stamp.Match(stamp);
        if (!m.Success)
        {
            return null;
        }
        int N(int group) => int.Parse(m.Groups[group].Value, NumberStyles.None, CultureInfo.InvariantCulture);
        int year = N(1), month = N(2), day = N(3), hour = N(4), minute = N(5), second = N(6), ms = N(7);
        if (year < 1 || month is < 1 or > 12 || day < 1 || day > DateTime.DaysInMonth(year, month) || hour > 23 || minute > 59 || second > 59)
        {
            return null;
        }
        var offset = 0;
        if (!m.Groups[8].Success)
        {
            int oh = N(10), om = N(11);
            if (oh > 23 || om > 59)
            {
                return null;
            }
            offset = (oh * 60 + om) * (m.Groups[9].Value == "-" ? -1 : 1);
        }
        try
        {
            return new DateTimeOffset(year, month, day, hour, minute, second, ms, TimeSpan.FromMinutes(offset));
        }
        catch (ArgumentException)
        {
            return null;
        }
    }

    // Reads a Go-quoted string at the start of s, as strconv.QuotedPrefix and Unquote do:
    // its value and how many characters the quoted string took.
    private static bool UnquotePrefix(string s, out string value, out int used)
    {
        var b = new StringBuilder();
        value = "";
        used = 0;
        var i = 1;
        while (i < s.Length)
        {
            var c = s[i];
            if (c == '"')
            {
                value = b.ToString();
                used = i + 1;
                return true;
            }
            if (c == '\n')
            {
                return false;
            }
            if (c != '\\')
            {
                b.Append(c);
                i++;
                continue;
            }
            if (i + 1 >= s.Length)
            {
                return false;
            }
            var e = s[i + 1];
            i += 2;
            switch (e)
            {
                case '"': b.Append('"'); continue;
                case '\\': b.Append('\\'); continue;
                case 'n': b.Append('\n'); continue;
                case 'r': b.Append('\r'); continue;
                case 't': b.Append('\t'); continue;
                case 'a': b.Append('\a'); continue;
                case 'b': b.Append('\b'); continue;
                case 'f': b.Append('\f'); continue;
                case 'v': b.Append('\v'); continue;
                case 'x' or 'u' or 'U':
                    var len = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                    if (i + len > s.Length || !int.TryParse(s.AsSpan(i, len), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var code) || code < 0)
                    {
                        return false;
                    }
                    if (e != 'x' && !Rune.IsValid(code))
                    {
                        return false;
                    }
                    b.Append(new Rune(code).ToString());
                    i += len;
                    continue;
                case >= '0' and <= '7':
                    if (i + 2 > s.Length || s[i] is < '0' or > '7' || s[i + 1] is < '0' or > '7')
                    {
                        return false;
                    }
                    var octal = (e - '0') * 64 + (s[i] - '0') * 8 + (s[i + 1] - '0');
                    if (octal > 255)
                    {
                        return false;
                    }
                    b.Append((char)octal);
                    i += 2;
                    continue;
                default:
                    return false;
            }
        }
        return false;
    }

    // Where the first " key=" starts in an unquoted message's line, or -1.
    private static int FirstField(string s)
    {
        for (var i = 0; i < s.Length; i++)
        {
            if (s[i] != ' ')
            {
                continue;
            }
            var word = s[(i + 1)..];
            var space = word.IndexOf(' ');
            if (space >= 0)
            {
                word = word[..space];
            }
            var eq = word.IndexOf('=');
            if (eq >= 0 && ValidKey(word[..eq]))
            {
                return i;
            }
        }
        return -1;
    }

    // Go's strings.Cut.
    private static bool Cut(string s, char sep, out string before, out string after)
    {
        var i = s.IndexOf(sep);
        if (i < 0)
        {
            before = s;
            after = "";
            return false;
        }
        before = s[..i];
        after = s[(i + 1)..];
        return true;
    }

    /// <summary>Reads a line written by <see cref="Format"/> (without its newline).</summary>
    /// <exception cref="FormatException">What doesn't fit the standard.</exception>
    public static Entry Parse(string line) =>
        TryParse(line, out var entry, out var error) ? entry! : throw new FormatException(error);

    /// <summary>Reads a line written by <see cref="Format"/>, or says what doesn't fit the standard.</summary>
    public static bool TryParse(string line, out Entry? entry, out string? error)
    {
        entry = null;
        error = Read(line, out entry);
        return error == null;
    }

    private static string? Read(string line, out Entry? entry)
    {
        entry = null;
        if (!Cut(line, ' ', out var stamp, out var rest))
        {
            return "no time";
        }
        if (ParseTime(stamp) is not { } time)
        {
            return $"time \"{stamp}\" isn't RFC 3339 with milliseconds and a zone";
        }
        var ok = Cut(rest, ' ', out var pidText, out rest);
        if (!ok || !pidText.StartsWith("pid=", StringComparison.Ordinal)
            || !int.TryParse(pidText.AsSpan(4), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var pid) || pid <= 0)
        {
            return "no pid=<n> after the time";
        }
        ok = Cut(rest, ' ', out var levelText, out rest);
        Level level;
        switch (levelText)
        {
            case "info": level = Level.Info; break;
            case "warn": level = Level.Warn; break;
            case "error": level = Level.Error; break;
            default: return $"level \"{levelText}\" isn't info, warn or error";
        }
        if (!ok)
        {
            return "no message";
        }
        string message;
        if (rest.StartsWith('"'))
        {
            if (!UnquotePrefix(rest, out message, out var used))
            {
                return "unterminated quoted message";
            }
            rest = rest[used..];
        }
        else
        {
            var i = FirstField(rest);
            var end = i >= 0 ? i : rest.Length;
            message = rest[..end];
            if (message.EndsWith(' '))
            {
                message = message[..^1];
            }
            rest = rest[end..];
        }
        var fields = new List<Field>();
        while (rest.Length > 0)
        {
            if (rest[0] != ' ')
            {
                return $"junk after the message: \"{rest}\"";
            }
            rest = rest[1..];
            if (!Cut(rest, '=', out var key, out var after) || !ValidKey(key))
            {
                return $"field \"{rest}\" isn't key=value";
            }
            string value;
            if (after.StartsWith('"'))
            {
                if (!UnquotePrefix(after, out value, out var used))
                {
                    return $"field {key}: unterminated quote";
                }
                rest = after[used..];
            }
            else
            {
                Cut(after, ' ', out value, out rest);
                if (rest.Length > 0)
                {
                    rest = " " + rest;
                }
                if (value.Length == 0)
                {
                    return $"field {key} has no value";
                }
            }
            fields.Add(new Field(key, value));
        }
        entry = new Entry(time, pid, level, message, fields);
        return null;
    }

    // Go's \b, ASCII's word boundary, before a word character; and anywhere.
    private const string WordStart = "(?<![A-Za-z0-9_])";
    private const string Boundary = "(?:(?<=[A-Za-z0-9_])(?![A-Za-z0-9_])|(?<![A-Za-z0-9_])(?=[A-Za-z0-9_]))";
    private const string Keep = "${1}" + Marker;

    // The estate's one list of credential shapes, the Go package's secrets exactly
    // (redact.go; both run testdata/redact.json): each keeps the text that names the
    // credential and replaces the credential itself.
    private static readonly (Regex Pattern, string Replacement)[] Secrets =
    [
        // A URL's query parameter: ?code=, &sig=, &access_token= and the like.
        (Secret(@"(?i)([?&](?:code|state|sig|signature|token|access_token|id_token|refresh_token|client_secret|password|passwd|secret|key|api_key|apikey|x-goog-signature|x-amz-signature)=)[^&\t\n\f\r ""']+"), Keep),
        // An Authorization header, with or without its scheme.
        (Secret(@"(?i)(authorization:[\t\n\f\r ]*(?:bearer|token|basic)?[\t\n\f\r ]*)[^\t\n\f\r '""]+"), Keep),
        (Secret(@"(?i)(bearer[\t\n\f\r ]+)[^\t\n\f\r '""]+"), Keep),
        (Secret(@"(?i)" + WordStart + @"(basic[\t\n\f\r ]+)[a-z0-9._~+/=-]{8,}"), Keep),
        // NAME=value and "name": "value", for the names credentials go by.
        (Secret(@"(?i)" + WordStart + @"((?:token|password|passwd|secret|api[_-]?key|access[_-]?key|client[_-]?secret|pat|bw_session)""?[\t\n\f\r ]*[=:][\t\n\f\r ]*""?)[^\t\n\f\r '""&]+"), Keep),
        // Any name ending in token or _secret, code_verifier and authorization, also in
        // single quotes, keeping an Authorization scheme; a value of digits alone stays.
        (Secret(@"(?i)" + WordStart + @"((?:[A-Za-z0-9_-]*(?:token|[_-]secret)|code[_-]?verifier|authorization)[""']?[\t\n\f\r ]*[=:][\t\n\f\r ]*[""']?(?:(?:bearer|token|basic)[\t\n\f\r ]+)?)[^\t\n\f\r '""&,;}]*[^\t\n\f\r '""&,;}0-9][^\t\n\f\r '""&,;}]*"), Keep),
        // A Cookie or Set-Cookie value, every cookie in it, to the end of the line or quote.
        (Secret(@"(?i)" + WordStart + @"((?:set-)?cookie[""']?[\t\n\f\r ]*[=:][\t\n\f\r ]*[""']?)[^\t\n\f\r '""][^\n\r'""]*"), Keep),
        // A command's flag: --token x, --password=x.
        (Secret(@"(?i)(--(?:token|password|secret|api-key|client-secret)[= ])[^\t\n\f\r '""]+"), Keep),
        // The user and password in a URL.
        (Secret(@"(://)[^/@\t\n\f\r '""]+@"), Keep + "@"),
        // A JWT: three parts, or two long ones.
        (Secret(@"eyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+|eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}"), Marker),
        // GitHub tokens, and OpenBao's (hvs., hvb., s., b.).
        (Secret(WordStart + @"(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}"), Marker),
        (Secret(WordStart + @"(?:hvs|hvb|s|b)\.[A-Za-z0-9_-]{24,}" + Boundary), Marker),
    ];

    private static Regex Secret(string pattern) => new(pattern, RegexOptions.CultureInvariant);

    /// <summary>
    /// text with what looks like a credential replaced by <see cref="Marker"/>, keeping the
    /// rest. Pass it any field or message holding text from outside the program before
    /// logging it: a command's output or arguments, another tool's error text, a URL, a
    /// header. Every home PC's everyday NAS sign-in can read the pile.
    /// </summary>
    public static string Redact(string text)
    {
        foreach (var (pattern, replacement) in Secrets)
        {
            text = pattern.Replace(text, replacement);
        }
        return text;
    }
}
