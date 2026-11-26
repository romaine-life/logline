package logline

import "regexp"

// Marker is what Redact writes in place of a credential.
const Marker = "***"

// secrets are the estate's one list of credential shapes (AHT-204): what services print
// inside otherwise useful lines (oauth2-proxy's sign-in code and state, Forgejo's signed
// artifact URLs) and what commands carry (headers, flags, NAME=value, passwords in URLs,
// token formats). Each keeps the text that names the credential and replaces the
// credential itself. The Rust crate holds the same list; testdata/redact.json is what
// both must do.
//
// Classes are spelled out in ASCII ([\t\n\f\r ] for a space, [A-Za-z0-9_] for a word
// character) so Go's and Rust's regular expressions read them the same way.
var secrets = []struct {
	pattern *regexp.Regexp
	keep    string
}{
	// A URL's query parameter: ?code=, &sig=, &access_token= and the like.
	{regexp.MustCompile(`(?i)([?&](?:code|state|sig|signature|token|access_token|id_token|refresh_token|client_secret|password|passwd|secret|key|api_key|apikey|x-goog-signature|x-amz-signature)=)[^&\t\n\f\r "']+`), "${1}" + Marker},
	// An Authorization header, with or without its scheme.
	{regexp.MustCompile(`(?i)(authorization:[\t\n\f\r ]*(?:bearer|token|basic)?[\t\n\f\r ]*)[^\t\n\f\r '"]+`), "${1}" + Marker},
	{regexp.MustCompile(`(?i)(bearer[\t\n\f\r ]+)[^\t\n\f\r '"]+`), "${1}" + Marker},
	{regexp.MustCompile(`(?i)\b(basic[\t\n\f\r ]+)[a-z0-9._~+/=-]{8,}`), "${1}" + Marker},
	// NAME=value and "name": "value", for the names credentials go by.
	{regexp.MustCompile(`(?i)\b((?:token|password|passwd|secret|api[_-]?key|access[_-]?key|client[_-]?secret|pat|bw_session)"?[\t\n\f\r ]*[=:][\t\n\f\r ]*"?)[^\t\n\f\r '"&]+`), "${1}" + Marker},
	// Any name ending in token or _secret (refresh_token, id_token, accessToken,
	// X-Auth-Token), code_verifier and authorization, in those forms or a Python or
	// JavaScript dict's single quotes, keeping an Authorization scheme. A value of digits
	// alone is a count or a page number, not a credential, and stays.
	{regexp.MustCompile(`(?i)\b((?:[A-Za-z0-9_-]*(?:token|[_-]secret)|code[_-]?verifier|authorization)["']?[\t\n\f\r ]*[=:][\t\n\f\r ]*["']?(?:(?:bearer|token|basic)[\t\n\f\r ]+)?)[^\t\n\f\r '"&,;}]*[^\t\n\f\r '"&,;}0-9][^\t\n\f\r '"&,;}]*`), "${1}" + Marker},
	// A Cookie or Set-Cookie value, every cookie in it, to the end of the line or quote.
	{regexp.MustCompile(`(?i)\b((?:set-)?cookie["']?[\t\n\f\r ]*[=:][\t\n\f\r ]*["']?)[^\t\n\f\r '"][^\n\r'"]*`), "${1}" + Marker},
	// A command's flag: --token x, --password=x.
	{regexp.MustCompile(`(?i)(--(?:token|password|secret|api-key|client-secret)[= ])[^\t\n\f\r '"]+`), "${1}" + Marker},
	// The user and password in a URL.
	{regexp.MustCompile(`(://)[^/@\t\n\f\r '"]+@`), "${1}" + Marker + "@"},
	// A JWT: three parts, or two long ones.
	{regexp.MustCompile(`eyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+|eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}`), Marker},
	// GitHub tokens, and OpenBao's (hvs., hvb., s., b.).
	{regexp.MustCompile(`\b(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}`), Marker},
	{regexp.MustCompile(`\b(?:hvs|hvb|s|b)\.[A-Za-z0-9_-]{24,}\b`), Marker},
}

// Redact replaces what looks like a credential in text with Marker, keeping the rest.
// Pass it any field or message holding text from outside the program before logging
// it: a command's output or arguments, another tool's error text, a URL, a header.
// Every home PC's everyday NAS sign-in can read the pile.
func Redact(text string) string {
	for _, s := range secrets {
		text = s.pattern.ReplaceAllString(text, s.keep)
	}
	return text
}
