package dev.lsmtree.server.engine;

/** One live pair returned by a range scan; both sides are raw engine bytes. */
public record KeyValue(byte[] key, byte[] value) {}
