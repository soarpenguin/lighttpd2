
#include <lighttpd/settings.h>

%%{
	machine ssi_parser;

	action mark {
	}

	action valueend {
	}

	WS = " "; # | "\t" | "\v" | "\r" | "\n";

	DQUOTE = '"';
	SQUOTE = "'";
	BQUOTE = "`";

	value = WS* "=" WS* (
		  ((ascii+ -- (cntrl | WS | DQUOTE | SQUOTE | BQUOTE | ">" | "-" | "=")) >~mark %valueend)
		| (DQUOTE (any*) >~mark %valueend :> DQUOTE)
		| (SQUOTE (any*) >~mark %valueend :> SQUOTE)
		| (BQUOTE (any*) >~mark %valueend :> BQUOTE)
		);

	elem =
		  ("config" (WS+ ("echomsg" value | "errmsg" value | "sizefmt" value | "timefmt" value))*)
		| ("echo" (WS+ ("var" value | "encoding" value))*)
		;

	element = "<!--#" WS* elem WS* "-->";

	plaintext = (any*) -- ("<!--#");

	content = plaintext (element plaintext)*;

	main := element;

	write data;
}%%
