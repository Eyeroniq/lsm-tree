package dev.lsmtree.server.engine;

/** The engine reported an error, such as an I/O failure or a corrupt table. */
public class LsmException extends RuntimeException {

  public LsmException(String message) {
    super(message);
  }

  public LsmException(String message, Throwable cause) {
    super(message, cause);
  }
}
