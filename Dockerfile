# Builds the C++ engine library and the Spring Boot service into one image.
#   docker build -t lsm-tree-server .
#   docker run -p 8080:8080 -v lsm-data:/data lsm-tree-server

FROM maven:3.9-eclipse-temurin-25 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY include include
COPY src src
COPY bindings bindings
RUN make shared CXX=g++
COPY server/pom.xml server/pom.xml
RUN cd server && mvn -B -q dependency:go-offline
COPY server/src server/src
RUN cd server && mvn -B -q package -DskipTests

FROM eclipse-temurin:25-jre
WORKDIR /app
COPY --from=build /src/build/liblsm.so /app/liblsm.so
COPY --from=build /src/server/target/lsm-tree-server-*.jar /app/app.jar
ENV LSM_DATA_DIR=/data \
    LSM_LIBRARY_PATH=/app/liblsm.so
VOLUME /data
EXPOSE 8080
ENTRYPOINT ["java", "--enable-native-access=ALL-UNNAMED", "-jar", "/app/app.jar"]
