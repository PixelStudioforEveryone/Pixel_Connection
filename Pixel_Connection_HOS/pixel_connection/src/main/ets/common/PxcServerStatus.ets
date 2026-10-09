import { http } from '@kit.NetworkKit';
import { pxcIsExampleServerAddress } from './PxcSettings';

interface PxcServerIdentity {
  product: string;
  id: string;
  name: string;
  platform: string;
  ready: boolean;
}
interface PxcHealthResponse { status: string; api: string; server?: PxcServerIdentity; }

export async function pxcServerDescription(apiUrl: string): Promise<string> {
  if (pxcIsExampleServerAddress(apiUrl)) { return '当前为示例服务器，请在高级设置填写自己的账号 API 和信令地址'; }
  if (!/^https?:\/\//.test(apiUrl)) { return '请输入有效的账号 API 地址'; }
  const client = http.createHttp();
  try {
    const response = await client.request(apiUrl.replace(/\/+$/, '') + '/api/v1/health', {
      method: http.RequestMethod.GET, connectTimeout: 3000, readTimeout: 3000,
      expectDataType: http.HttpDataType.STRING, usingCache: false
    });
    const raw: string = response.result as string;
    if (response.responseCode !== 200 || raw.length > 16384) { return '服务器暂时不可达'; }
    const result: PxcHealthResponse = JSON.parse(raw) as PxcHealthResponse;
    if (result.status !== 'ok' || result.api !== 'v1') { return '此地址不是 PixelConnection 账号服务'; }
    const server = result.server;
    if (server === undefined || server.product !== 'PixelConnection' || !server.id) { return '服务可达；旧版服务器未提供来源信息'; }
    return `服务器：${server.name} · ${server.platform}\n服务器标识：${server.id}${server.ready ? '' : '\n服务器仍在启动中'}`;
  } catch (_) { return '服务器暂时不可达'; }
  finally { client.destroy(); }
}
