# Definition header contract

Header authoring, loading, and writing share the policy below. The rationale
is in the [definition design notes](../design-notes.md#definitions). Start with
[header draft APIs](../../src/backend/definition/definition_header_fields.h),
[parser helpers](../../src/backend/definition/parser_utils.h), and the
[writer](../../src/backend/definition/definition_writer.h).

## Authoring and normalization

`definition_header_fields` owns imported XML header extraction into `DefinitionHeaderDraft` and conversion into
`DefinitionHeaderInput`. A draft uses named fields and keeps editable address
text until submission. Qt adapters populate a fixed form and read named editor
references; they convert QString/UTF-8 and display errors. Parallel name/value
lists and object-name lookup are not part of the authoring boundary. The portable
backend and header/form test closure have no QtXml dependency.

Header import, validated loading, and writing share a permissive-read,
canonical-write policy. Direct text and CDATA children are concatenated in order;
comments and processing instructions contribute no text. Nested elements in
recognized header text fields are rejected with source/field context rather than
flattened. Surrounding Unicode White_Space is trimmed from identifiers, include
references, and scalar metadata; internal spaces and both notes fields retain
their formatting. Table text and protocol numeric parsing keep their own existing
contracts; these header rules do not redefine them.

Hexadecimal header addresses accept surrounding whitespace, an optional leading
plus, and `0x`/`0X`; negatives, embedded whitespace, trailing junk, and uint64
overflow fail. Missing or whitespace-only addresses are absent. Writers produce
lowercase `0x` hexadecimal or remove absent address elements. Uint64 acceptance
and blank-as-absent are deliberate application choices, rather than claims that
both reference tools use these exact ranges or validation rules.

Both EcuFlash and RomRaider use `ParseRomHeader` for validated identity assembly.
Draft import permits missing identities and does not parse tables; loading and
writer submission enforce their required fields. Malformed XML, multiple document
roots, unsupported import roots, and nested header markup produce errors before
an authoring dialog opens. Import accepts a `rom` root or selects the first direct
`rom` child of `roms`; it does not search arbitrary wrappers. File-byte loading
uses encoding autodetection; already decoded form strings are explicitly UTF-8.
Raw file bytes reach the portable extractor before any QString conversion, so
UTF-16 imports use the same autodetection as loading and rewriting. Notes editors
use plain text to preserve literal markup from escaped XML or CDATA. The XML
parser remains non-validating, without custom DTD expansion or external resource
loading.

`kEditableMetadataFields` shares the nine editable field mappings among draft
extraction, parsed metadata, normalization, and serialization. Parser metadata
also reads `filesize` and `romid/notes`; the form's `include` and notes come from
the ROM. Writers normalize direct backend callers as well as form submissions,
validate canonical required identities, replace writable fields with single text
values, and preserve unrelated XML content. Successful catalog submissions use
the same canonical XML ID as the generated document.

## Reference evidence

The reference implementations inform this policy without requiring their
incidental parser-library behaviour. RomRaider's `DOMHelper.unmarshallText`
concatenates direct TEXT_NODE children without trimming; its RomID parser uses
that helper for identities and metadata. EcuFlash's decompiled XML end-element
handler (`FUN_004990f0`) trims accumulated text through
`FUN_00ab3a70`/`FUN_00aa60f0`/`FUN_00aa61b0`; its header handler
(`FUN_0041a1e0`) parses base-16 addresses through `FUN_00aa5d80`, which rejects
values above 32 bits. ROM-level include/notes and the separate romid handler
appear in `FUN_0041dc00` and the preceding ROM field handler. These addresses
refer to the locally supplied Ghidra dump; nested-text and DTD behaviour have not
been established by that analysis.

Strict duplicate detection, candidate identity validation before selected-address
parsing, and first-ROM authoring selection remain explicit application policies.
They are not documented as RomRaider requirements: its reference loader parses
candidate headers before matching and returns the first match. Required identity
checks, contextual errors, and writing/registration decisions stay separate from
partial draft extraction.
