package dev.lsmtree.server.engine;

/** The engine rejected the caller's input, such as an empty or oversized key. */
public class LsmInvalidArgumentException extends LsmException {

  public LsmInvalidArgumentException(String message) {
    super(message);
  }
}
