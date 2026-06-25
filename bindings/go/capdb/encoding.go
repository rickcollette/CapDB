package capdb

import (
	"database/sql/driver"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// timeLayout matches CapDB's timestamp format for round-trip compatibility.
const timeLayout = "2006-01-02 15:04:05.999999999-07:00"

// substitute replaces positional `?` placeholders with SQL literals of arguments.
// It respects string literals, comments, and bracket-quoted identifiers.
func substitute(query string, args []driver.Value) (string, error) {
	if strings.IndexByte(query, 0) >= 0 {
		return "", fmt.Errorf("capdb: query contains a NUL byte (not supported)")
	}
	var b strings.Builder
	b.Grow(len(query) + 16*len(args))

	argi := 0
	maxArg := 0
	i := 0
	n := len(query)
	for i < n {
		c := query[i]
		switch {
		case c == '\'':
			j := i + 1
			for j < n {
				if query[j] == '\'' {
					if j+1 < n && query[j+1] == '\'' {
						j += 2
						continue
					}
					j++
					break
				}
				j++
			}
			b.WriteString(query[i:j])
			i = j
		case c == '"':
			j := i + 1
			for j < n && query[j] != '"' {
				j++
			}
			if j < n {
				j++
			}
			b.WriteString(query[i:j])
			i = j
		case c == '[':
			j := i + 1
			for j < n && query[j] != ']' {
				j++
			}
			if j < n {
				j++
			}
			b.WriteString(query[i:j])
			i = j
		case c == '-' && i+1 < n && query[i+1] == '-':
			j := i + 2
			for j < n && query[j] != '\n' {
				j++
			}
			b.WriteString(query[i:j])
			i = j
		case c == '/' && i+1 < n && query[i+1] == '*':
			j := i + 2
			for j+1 < n && !(query[j] == '*' && query[j+1] == '/') {
				j++
			}
			if j+1 < n {
				j += 2
			}
			b.WriteString(query[i:j])
			i = j
		case c == '?':
			if i+1 < n && query[i+1] >= '0' && query[i+1] <= '9' {
				j := i + 1
				for j < n && query[j] >= '0' && query[j] <= '9' {
					j++
				}
				idx64, err := strconv.ParseInt(query[i+1:j], 10, 32)
				if err != nil || idx64 <= 0 || int(idx64) > len(args) {
					return "", fmt.Errorf("capdb: not enough arguments for placeholders (have %d)", len(args))
				}
				lit, err := encodeLiteral(args[int(idx64)-1])
				if err != nil {
					return "", err
				}
				b.WriteString(lit)
				if int(idx64) > maxArg {
					maxArg = int(idx64)
				}
				i = j
				continue
			}
			if argi >= len(args) {
				return "", fmt.Errorf("capdb: not enough arguments for placeholders (have %d)", len(args))
			}
			lit, err := encodeLiteral(args[argi])
			if err != nil {
				return "", err
			}
			b.WriteString(lit)
			argi++
			if argi > maxArg {
				maxArg = argi
			}
			i++
		default:
			b.WriteByte(c)
			i++
		}
	}
	if maxArg != len(args) {
		return "", fmt.Errorf("capdb: %d arguments provided but %d placeholders consumed", len(args), maxArg)
	}
	return b.String(), nil
}

// encodeLiteral renders a driver.Value as a CapDB SQL literal.
func encodeLiteral(v driver.Value) (string, error) {
	switch t := v.(type) {
	case nil:
		return "NULL", nil
	case int64:
		return strconv.FormatInt(t, 10), nil
	case float64:
		return strconv.FormatFloat(t, 'g', -1, 64), nil
	case bool:
		if t {
			return "1", nil
		}
		return "0", nil
	case []byte:
		return encodeBlob(t), nil
	case string:
		if strings.IndexByte(t, 0) >= 0 {
			return "", fmt.Errorf("capdb: string argument contains a NUL byte (not supported)")
		}
		return encodeString(t), nil
	case time.Time:
		return encodeString(t.Format(timeLayout)), nil
	default:
		return "", fmt.Errorf("capdb: unsupported argument type %T", v)
	}
}

func encodeString(s string) string {
	return "'" + strings.ReplaceAll(s, "'", "''") + "'"
}

func encodeBlob(b []byte) string {
	const hexdigits = "0123456789abcdef"
	var sb strings.Builder
	sb.Grow(len(b)*2 + 3)
	sb.WriteString("x'")
	for _, c := range b {
		sb.WriteByte(hexdigits[c>>4])
		sb.WriteByte(hexdigits[c&0x0f])
	}
	sb.WriteByte('\'')
	return sb.String()
}

func countPlaceholders(query string) int {
	count := 0
	i := 0
	n := len(query)
	for i < n {
		c := query[i]
		switch {
		case c == '\'':
			j := i + 1
			for j < n {
				if query[j] == '\'' {
					if j+1 < n && query[j+1] == '\'' {
						j += 2
						continue
					}
					j++
					break
				}
				j++
			}
			i = j
		case c == '"':
			j := i + 1
			for j < n && query[j] != '"' {
				j++
			}
			i = j + 1
		case c == '[':
			j := i + 1
			for j < n && query[j] != ']' {
				j++
			}
			i = j + 1
		case c == '-' && i+1 < n && query[i+1] == '-':
			j := i + 2
			for j < n && query[j] != '\n' {
				j++
			}
			i = j
		case c == '/' && i+1 < n && query[i+1] == '*':
			j := i + 2
			for j+1 < n && !(query[j] == '*' && query[j+1] == '/') {
				j++
			}
			i = j + 2
		case c == '?':
			if i+1 < n && query[i+1] >= '0' && query[i+1] <= '9' {
				j := i + 1
				for j < n && query[j] >= '0' && query[j] <= '9' {
					j++
				}
				idx64, err := strconv.ParseInt(query[i+1:j], 10, 32)
				if err == nil && int(idx64) > count {
					count = int(idx64)
				}
				i = j
				continue
			}
			count++
			i++
		default:
			i++
		}
	}
	return count
}
