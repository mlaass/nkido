/**
 * Default URI handler registration. Importing this file (or any module
 * that depends on it, e.g. `file-loader.ts`) wires the standard set of
 * handlers onto the singleton resolver.
 */
import { uriResolver } from '../uri-resolver';
import { httpHandler, httpsHandler } from './http-handler';
import { githubHandler } from './github-handler';
import { blobHandler } from './blob-handler';
import { bundledHandler } from './bundled-handler';
import { idbHandler } from './idb-handler';

let registered = false;

export function registerDefaultHandlers(): void {
	if (registered) return;
	uriResolver.register(httpHandler);
	uriResolver.register(httpsHandler);
	uriResolver.register(githubHandler);
	uriResolver.register(blobHandler);
	uriResolver.register(bundledHandler);
	uriResolver.register(idbHandler);
	registered = true;
}

registerDefaultHandlers();

export { httpHandler, httpsHandler, githubHandler, blobHandler, bundledHandler, idbHandler };
