# RHI implementation

RHI currently defines its backend-neutral contracts in public headers. Concrete device creation
and native API implementation remain in the selected backend and renderer composition layer.
