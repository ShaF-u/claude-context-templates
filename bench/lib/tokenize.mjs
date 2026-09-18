// Rough estimate (chars/4), not a real tokenizer. Used identically on
// both sides of every comparison in this harness, so the *ratio*
// between naive and core stays meaningful even though the absolute
// numbers are approximate.
export function estimateTokens(text) {
  if (!text) return 0;
  return Math.ceil(text.length / 4);
}
