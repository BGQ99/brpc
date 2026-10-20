package prototype.echo;

/** Java business code. No JNI, bthread, or Protobuf types. */
public interface EchoService {
    byte[] echo(byte[] payload) throws Exception;
}
