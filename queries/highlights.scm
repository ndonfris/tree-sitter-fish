[(double_quote_string) (single_quote_string)] @string
(escape_sequence) @string.escape

(comment) @comment

[(integer) (float)] @number

[
  "&&"
  "||"
  "|"
  "&|"
  "2>|"
  "&"
  ".."
  (direction)
  (stream_redirect)
] @operator

(command name: (word) @function)

; Match the opening bracket of the [ command independently.
(command
  name: (word) @punctuation.bracket
  (#match? @punctuation.bracket "^\\[$"))

; Match all arguments of test and [ in one query match. A separate match for
; each operator repeats the command-name capture, causing the highlighter to
; discard earlier operators when a command contains several of them.
(command
  name: (word) @_test_command
  [
    argument: (word) @operator
    argument: (_) @_test_operand
    redirect: (_) @_test_redirect
  ]*
  (#any-of? @_test_command "test" "[")
  (#any-of? @operator
    "=" "!="
    "-a" "-o"
    "-b" "-c" "-d" "-e" "-f" "-g" "-G" "-k" "-L" "-O"
    "-p" "-r" "-s" "-S" "-t" "-u" "-w" "-x"
    "-ef" "-nt" "-ot"
    "-n" "-z"
    "-eq" "-ne" "-gt" "-ge" "-lt" "-le")
  (#not-any-of? @_test_operand
    "=" "!="
    "-a" "-o"
    "-b" "-c" "-d" "-e" "-f" "-g" "-G" "-k" "-L" "-O"
    "-p" "-r" "-s" "-S" "-t" "-u" "-w" "-x"
    "-ef" "-nt" "-ot"
    "-n" "-z"
    "-eq" "-ne" "-gt" "-ge" "-lt" "-le"))

(variable_expansion) @constant

(command_substitution "$" @punctuation.special)
(command_substitution ["(" ")"] @punctuation.bracket)
(list_element_access  ["[" "]"] @punctuation.bracket)
(brace_expansion      ["{" "}"] @punctuation.bracket)
(begin_statement      ["{" "}"] @punctuation.bracket)

"," @punctuation.delimiter

(function_definition name: [(word) (concatenation)] @function)

[
 "switch"
 "case"
 "in"
 "begin"
 "function"
 "if"
 "else"
 "end"
 "while"
 "for"
 "not"
 "!"
 "and"
 "or"
 "return"
 (break)
 (continue)
] @keyword
