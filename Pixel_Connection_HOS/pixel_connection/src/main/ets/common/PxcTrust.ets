import { fileIo } from '@kit.CoreFileKit';

// OpenSSL in the native HAP cannot assume a desktop /etc/ssl trust store.
// Ship the verified Mozilla bundle and refresh the sandbox copy on every launch.
export function preparePxcTrust(context: Context): void {
  const data = context.resourceManager.getRawFileContentSync('pxc-ca-bundle.pem');
  const file = fileIo.openSync(context.filesDir + '/pxc-ca-bundle.pem',
    fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
  try {
    fileIo.writeSync(file.fd, data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
  } finally { fileIo.closeSync(file); }
}
