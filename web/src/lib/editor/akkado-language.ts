import { StreamLanguage, HighlightStyle, syntaxHighlighting } from '@codemirror/language';
import { tags } from '@lezer/highlight';
import { BUILTIN_NAMES, ALIAS_NAMES } from './generated/syntax-builtins';

const KEYWORDS = new Set(['true', 'false', 'fn', 'as', 'match', 'post']);
const BUILTINS = new Set([...BUILTIN_NAMES, ...ALIAS_NAMES]);

const akkadoLang = StreamLanguage.define({
	startState() {
		return {};
	},
	token(stream) {
		// 1. Whitespace
		if (stream.eatSpace()) return null;

		// 2. Line comments
		if (stream.match('//')) {
			stream.skipToEnd();
			return 'comment';
		}

		// 3. Strings
		if (stream.match('"')) {
			while (!stream.eol()) {
				const ch = stream.next();
				if (ch === '\\') {
					stream.next(); // skip escaped char
				} else if (ch === '"') {
					break;
				}
			}
			return 'string';
		}

		// 4. Numbers: integer, float, or .digit
		if (stream.match(/^[0-9]+(\.[0-9]*)?([eE][+-]?[0-9]+)?/) ||
			stream.match(/^\.[0-9]+([eE][+-]?[0-9]+)?/)) {
			return 'number';
		}

		// 5. Directives: $word
		if (stream.match(/^\$\w+/)) {
			return 'meta';
		}

		// 6. Hole
		if (stream.peek() === '%' && !stream.match(/^%\w/, false)) {
			stream.next();
			return 'keyword';
		}

		// 7. Rest
		if (stream.peek() === '~') {
			stream.next();
			return 'atom';
		}

		// 8. Multi-char operators
		if (stream.match('|>') || stream.match('->') ||
			stream.match('==') || stream.match('!=') ||
			stream.match('<=') || stream.match('>=') ||
			stream.match('&&') || stream.match('||')) {
			return 'operator';
		}

		// 9. Single-char operators
		const ch = stream.peek();
		if (ch && '+-*/^=<>.@!?|'.includes(ch)) {
			stream.next();
			return 'operator';
		}

		// 10. Identifiers and keywords
		if (stream.match(/^[a-zA-Z_]\w*/)) {
			const word = stream.current();
			if (KEYWORDS.has(word)) return 'keyword';
			if (BUILTINS.has(word)) return 'builtin';
			return null;
		}

		// 11. Any other character
		stream.next();
		return null;
	}
});

const akkadoHighlightStyle = HighlightStyle.define([
	{ tag: tags.keyword, color: 'var(--syntax-keyword)' },
	{ tag: tags.string, color: 'var(--syntax-string)' },
	{ tag: tags.number, color: 'var(--syntax-number)' },
	{ tag: tags.comment, color: 'var(--syntax-comment)', fontStyle: 'italic' },
	{ tag: tags.standard(tags.variableName), color: 'var(--syntax-function)' },
	{ tag: tags.operator, color: 'var(--syntax-operator)' },
	{ tag: tags.meta, color: 'var(--syntax-keyword)' },
	{ tag: tags.atom, color: 'var(--syntax-number)' }
]);

export function akkadoLanguage() {
	return [akkadoLang, syntaxHighlighting(akkadoHighlightStyle)];
}
