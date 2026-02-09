/**
 * File loading error types
 */

export type FileErrorCode = 'not_found' | 'network' | 'invalid_format' | 'too_large' | 'aborted';

export class FileLoadError extends Error {
	code: FileErrorCode;

	constructor(code: FileErrorCode, message: string) {
		super(message);
		this.name = 'FileLoadError';
		this.code = code;
	}
}
