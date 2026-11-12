package dev.lsmtree.server;

import io.swagger.v3.oas.annotations.OpenAPIDefinition;
import io.swagger.v3.oas.annotations.info.Info;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;

@SpringBootApplication
@OpenAPIDefinition(info = @Info(
    title = "LSM-tree key-value API",
    version = "v1",
    description = "HTTP access to a crash-safe C++ LSM-tree storage engine. "
        + "Keys and values are UTF-8 text; a write returns only after it is "
        + "fsynced to the write-ahead log."))
public class LsmServerApplication {

  public static void main(String[] args) {
    SpringApplication.run(LsmServerApplication.class, args);
  }
}
