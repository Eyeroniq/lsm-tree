package dev.lsmtree.server.api;

import dev.lsmtree.server.engine.LsmException;
import dev.lsmtree.server.engine.LsmInvalidArgumentException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.HttpStatus;
import org.springframework.http.ProblemDetail;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;

/** Maps engine failures to RFC 9457 problem responses. */
@RestControllerAdvice
public class ApiExceptionHandler {

  private static final Logger log = LoggerFactory.getLogger(ApiExceptionHandler.class);

  @ExceptionHandler({LsmInvalidArgumentException.class, IllegalArgumentException.class})
  public ProblemDetail badRequest(RuntimeException exception) {
    return ProblemDetail.forStatusAndDetail(HttpStatus.BAD_REQUEST, exception.getMessage());
  }

  @ExceptionHandler(LsmException.class)
  public ProblemDetail engineFailure(LsmException exception) {
    log.error("Storage engine failure", exception);
    ProblemDetail problem = ProblemDetail.forStatusAndDetail(
        HttpStatus.INTERNAL_SERVER_ERROR, exception.getMessage());
    problem.setTitle("Storage engine error");
    return problem;
  }
}
