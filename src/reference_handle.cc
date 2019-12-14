#include "reference_handle.h"
#include "external_copy.h"
#include "isolate/run_with_timeout.h"
#include "isolate/three_phase_task.h"
#include "transferable.h"
#include <array>

using namespace v8;
using std::shared_ptr;
using std::unique_ptr;

namespace ivm {
namespace {

using TypeOf = detail::ReferenceData::TypeOf;
auto InferTypeOf(Local<Value> value) -> TypeOf {
	if (value->IsNull()) {
		return TypeOf::Null;
	} else if (value->IsUndefined()) {
		return TypeOf::Undefined;
	} else if (value->IsNumber()) {
		return TypeOf::Number;
	} else if (value->IsString()) {
		return TypeOf::String;
	} else if (value->IsBoolean()) {
		return TypeOf::Boolean;
	} else if (value->IsFunction()) {
		return TypeOf::Function;
	} else {
		return TypeOf::Object;
	}
}

/**
 * The return value for .derefInto()
 */
class DereferenceHandleTransferable : public Transferable {
	public:
		DereferenceHandleTransferable(shared_ptr<IsolateHolder> isolate, RemoteHandle<v8::Value> reference) :
			isolate{std::move(isolate)}, reference{std::move(reference)} {}

		auto TransferIn() -> v8::Local<v8::Value> final {
			if (isolate == IsolateEnvironment::GetCurrentHolder()) {
				return Deref(reference);
			} else {
				throw js_type_error("Cannot dereference this into target isolate");
			}
		}

	private:
		shared_ptr<IsolateHolder> isolate;
		RemoteHandle<v8::Value> reference;
};

class DereferenceHandle : public TransferableHandle {
	public:
		DereferenceHandle(shared_ptr<IsolateHolder> isolate, RemoteHandle<v8::Value> reference) :
			isolate{std::move(isolate)}, reference{std::move(reference)} {}

		static auto Definition() -> v8::Local<v8::FunctionTemplate> {
			return Inherit<TransferableHandle>(MakeClass("Dereference", nullptr));
		}

		auto TransferOut() -> std::unique_ptr<Transferable> final {
			if (!reference) {
				throw js_generic_error("The return value of `derefInto()` should only be used once");
			}
			return std::make_unique<DereferenceHandleTransferable>(std::move(isolate), std::move(reference));
		}

	private:
		shared_ptr<IsolateHolder> isolate;
		RemoteHandle<v8::Value> reference;
};

} // anonymous namespace

namespace detail {

ReferenceData::ReferenceData(Local<Value> value) :
	isolate{IsolateEnvironment::GetCurrentHolder()},
	reference{RemoteHandle<Value>(value)},
	context{RemoteHandle<Context>(Isolate::GetCurrent()->GetCurrentContext())},
	type_of{InferTypeOf(value)} {}

ReferenceData::ReferenceData(
	shared_ptr<IsolateHolder> isolate,
	RemoteHandle<Value> reference,
	RemoteHandle<Context> context,
	TypeOf type_of
) :
	isolate{std::move(isolate)},
	reference{std::move(reference)},
	context{std::move(context)},
	type_of{type_of} {}

} // namespace detail

/**
 * ReferenceHandle implementation
 */
auto ReferenceHandle::Definition() -> Local<FunctionTemplate> {
	return Inherit<TransferableHandle>(MakeClass(
		"Reference", ConstructorFunction<decltype(&New), &New>{},
		"deref", MemberFunction<decltype(&ReferenceHandle::Deref), &ReferenceHandle::Deref>{},
		"derefInto", MemberFunction<decltype(&ReferenceHandle::DerefInto), &ReferenceHandle::DerefInto>{},
		"release", MemberFunction<decltype(&ReferenceHandle::Release), &ReferenceHandle::Release>{},
		"copy", MemberFunction<decltype(&ReferenceHandle::Copy<1>), &ReferenceHandle::Copy<1>>{},
		"copySync", MemberFunction<decltype(&ReferenceHandle::Copy<0>), &ReferenceHandle::Copy<0>>{},
		"get", MemberFunction<decltype(&ReferenceHandle::Get<1>), &ReferenceHandle::Get<1>>{},
		"getSync", MemberFunction<decltype(&ReferenceHandle::Get<0>), &ReferenceHandle::Get<0>>{},
		"set", MemberFunction<decltype(&ReferenceHandle::Set<1>), &ReferenceHandle::Set<1>>{},
		"setIgnored", MemberFunction<decltype(&ReferenceHandle::Set<2>), &ReferenceHandle::Set<2>>{},
		"setSync", MemberFunction<decltype(&ReferenceHandle::Set<0>), &ReferenceHandle::Set<0>>{},
		"apply", MemberFunction<decltype(&ReferenceHandle::Apply<1>), &ReferenceHandle::Apply<1>>{},
		"applyIgnored", MemberFunction<decltype(&ReferenceHandle::Apply<2>), &ReferenceHandle::Apply<2>>{},
		"applySync", MemberFunction<decltype(&ReferenceHandle::Apply<0>), &ReferenceHandle::Apply<0>>{},
		"applySyncPromise", MemberFunction<decltype(&ReferenceHandle::Apply<4>), &ReferenceHandle::Apply<4>>{},
		"typeof", MemberAccessor<decltype(&ReferenceHandle::TypeOfGetter), &ReferenceHandle::TypeOfGetter>{}
	));
}

auto ReferenceHandle::New(Local<Value> value) -> unique_ptr<ReferenceHandle> {
	return std::make_unique<ReferenceHandle>(value);
}

auto ReferenceHandle::TransferOut() -> unique_ptr<Transferable> {
	return std::make_unique<ReferenceHandleTransferable>(*this);
}

/**
 * Getter for typeof property.
 */
auto ReferenceHandle::TypeOfGetter() -> Local<Value> {
	CheckDisposed();
	switch (type_of) {
		case TypeOf::Null:
			return v8_string("null");
		case TypeOf::Undefined:
			return v8_string("undefined");
		case TypeOf::Number:
			return v8_string("number");
		case TypeOf::String:
			return v8_string("string");
		case TypeOf::Boolean:
			return v8_string("boolean");
		case TypeOf::Object:
			return v8_string("object");
		case TypeOf::Function:
			return v8_string("function");
	}
	std::terminate();
}

/**
 * Attempt to return this handle to the current context.
 */
auto ReferenceHandle::Deref(MaybeLocal<Object> maybe_options) -> Local<Value> {
	CheckDisposed();
	if (isolate.get() != IsolateEnvironment::GetCurrentHolder().get()) {
		throw js_type_error("Cannot dereference this from current isolate");
	}
	bool release = false;
	Local<Object> options;
	if (maybe_options.ToLocal(&options)) {
		release = IsOptionSet(Isolate::GetCurrent()->GetCurrentContext(), options, "release");
	}
	Local<Value> ret = ivm::Deref(reference);
	if (release) {
		Release();
	}
	return ret;
}

/**
 * Return a handle which will dereference itself when passing into another isolate.
 */
auto ReferenceHandle::DerefInto(MaybeLocal<Object> maybe_options) -> Local<Value> {
	CheckDisposed();
	bool release = false;
	Local<Object> options;
	if (maybe_options.ToLocal(&options)) {
		release = IsOptionSet(Isolate::GetCurrent()->GetCurrentContext(), options, "release");
	}
	Local<Value> ret = ClassHandle::NewInstance<DereferenceHandle>(isolate, reference);
	if (release) {
		Release();
	}
	return ret;
}

/**
 * Release this reference.
 */
auto ReferenceHandle::Release() -> Local<Value> {
	CheckDisposed();
	isolate.reset();
	reference = {};
	context = {};
	return Undefined(Isolate::GetCurrent());
}

/**
 * Call a function, like Function.prototype.apply
 */
class ApplyRunner : public ThreePhaseTask {
	public:
		ApplyRunner(
			ReferenceHandle& that,
			MaybeLocal<Value> recv_handle,
			MaybeLocal<Array> maybe_arguments,
			MaybeLocal<Object> maybe_options
		) :	context{that.context}, reference{that.reference}
		{
			that.CheckDisposed();

			// Get receiver, holder, this, whatever
			Local<Value> recv_local;
			if (recv_handle.ToLocal(&recv_local)) {
				recv = Transferable::TransferOut(recv_local);
			}

			// Get run options
			Transferable::Options arguments_transfer_options;
			Local<Context> context = Isolate::GetCurrent()->GetCurrentContext();
			Local<Object> options;
			if (maybe_options.ToLocal(&options)) {
				Local<Value> timeout_handle = Unmaybe(options->Get(context, v8_string("timeout")));
				if (!timeout_handle->IsUndefined()) {
					if (!timeout_handle->IsUint32()) {
						throw js_type_error("`timeout` must be integer");
					}
					timeout = timeout_handle.As<Uint32>()->Value();
				}

				Local<Value> arguments_transfer_handle = Unmaybe(options->Get(context, v8_string("arguments")));
				if (!arguments_transfer_handle->IsUndefined()) {
					if (!arguments_transfer_handle->IsObject()) {
						throw js_type_error("`arguments` must be object");
					}
					arguments_transfer_options = Transferable::Options{arguments_transfer_handle.As<Object>()};
				}

				Local<Value> return_transfer_handle = Unmaybe(options->Get(context, v8_string("return")));
				if (!return_transfer_handle->IsUndefined()) {
					if (!return_transfer_handle->IsObject()) {
						throw js_type_error("`return` must be object");
					}
					return_transfer_options = Transferable::Options{return_transfer_handle.As<Object>(), Transferable::Options::Type::Reference};
				}
			}

			// Externalize all arguments
			Local<Object> arguments;
			if (maybe_arguments.ToLocal(&arguments)) {
				Local<Array> keys = Unmaybe(arguments->GetOwnPropertyNames(context));
				argv.reserve(keys->Length());
				for (uint32_t ii = 0; ii < keys->Length(); ++ii) {
					Local<Uint32> key = Unmaybe(Unmaybe(keys->Get(context, ii))->ToArrayIndex(context));
					if (key->Value() != ii) {
						throw js_type_error("Invalid `arguments` array");
					}
					argv.push_back(Transferable::TransferOut(Unmaybe(arguments->Get(context, key)), arguments_transfer_options));
				}
			}
		}

		void Phase2() final {
			// Invoke in the isolate
			Local<Context> context_handle = Deref(context);
			Context::Scope context_scope{context_handle};
			Local<Value> fn = Deref(reference);
			if (!fn->IsFunction()) {
				throw js_type_error("Reference is not a function");
			}
			std::vector<Local<Value>> argv_inner = TransferArguments();
			Local<Value> recv_inner = recv->TransferIn();
			Local<Value> result = RunWithTimeout(timeout,
				[&fn, &context_handle, &recv_inner, &argv_inner]() {
					return fn.As<Function>()->Call(context_handle, recv_inner, argv_inner.size(), argv_inner.empty() ? nullptr : &argv_inner[0]);
				}
			);
			ret = Transferable::TransferOut(result, return_transfer_options);
		}

		bool Phase2Async(Scheduler::AsyncWait& wait) final {
			// Same as regular `Phase2()` but if it returns a promise we will wait on it
			if (!(return_transfer_options == Transferable::Options{})) {
				throw js_type_error("`return` options are not available for `applySyncPromise`");
			}
			Local<Context> context_handle = Deref(context);
			Context::Scope context_scope{context_handle};
			Local<Value> fn = Deref(reference);
			if (!fn->IsFunction()) {
				throw js_type_error("Reference is not a function");
			}
			Local<Value> recv_inner = recv->TransferIn();
			std::vector<Local<Value>> argv_inner = TransferArguments();
			Local<Value> value = RunWithTimeout(
				timeout,
				[&fn, &context_handle, &recv_inner, &argv_inner]() {
					return fn.As<Function>()->Call(context_handle, recv_inner, argv_inner.size(), argv_inner.empty() ? nullptr : &argv_inner[0]);
				}
			);
			if (value->IsPromise()) {
				Isolate* isolate = Isolate::GetCurrent();
				// This is only called from the default isolate, so we don't need an IsolateSpecific
				static Persistent<Function> callback_persistent{isolate, CompileAsyncWrapper()};
				Local<Function> callback_fn = Deref(callback_persistent);
				did_finish = std::make_shared<bool>(false);
				std::array<Local<Value>, 3> argv;
				argv[0] = External::New(isolate, reinterpret_cast<void*>(this));
				argv[1] = External::New(isolate, reinterpret_cast<void*>(new shared_ptr<bool>(did_finish)));
				argv[2] = value;
				async_wait = &wait;
				Unmaybe(callback_fn->Call(context_handle, callback_fn, 3, &argv.front()));
				return true;
			} else {
				ret = Transferable::TransferOut(value, return_transfer_options);
				return false;
			}
		}

		auto Phase3() -> Local<Value> final {
			if (did_finish && !*did_finish) {
				*did_finish = true;
				throw js_generic_error("Script execution timed out.");
			} else if (async_error) {
				Isolate::GetCurrent()->ThrowException(async_error->CopyInto());
				throw js_runtime_error();
			} else {
				return ret->TransferIn();
			}
		}

	private:
		/**
		 * This is an internal callback that will be called after a Promise returned from
		 * `applySyncPromise` has resolved
		 */
		static void AsyncCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
			// It's possible the invocation timed out, in which case the ApplyRunner will be dead. The
			// shared_ptr<bool> here will be marked as true and we can exit early.
			unique_ptr<shared_ptr<bool>> did_finish{reinterpret_cast<shared_ptr<bool>*>(info[1].As<External>()->Value())};
			if (**did_finish) {
				return;
			}
			ApplyRunner& self = *reinterpret_cast<ApplyRunner*>(info[0].As<External>()->Value());
			if (info.Length() == 3) {
				// Resolved
				FunctorRunners::RunCatchExternal(IsolateEnvironment::GetCurrent()->DefaultContext(), [&self, &info]() {
					self.ret = Transferable::TransferOut(info[2]);
				}, [&self](unique_ptr<ExternalCopy> error) {
					self.async_error = std::move(error);
				});
			} else {
				// Rejected
				self.async_error = ExternalCopy::CopyIfPrimitiveOrError(info[3]);
				if (!self.async_error) {
					self.async_error = std::make_unique<ExternalCopyError>(ExternalCopyError::ErrorType::Error,
						"An object was thrown from supplied code within isolated-vm, but that object was not an instance of `Error`."
					);
				}
			}
			*self.did_finish = true;
			self.async_wait->Wake();
		}

		/**
		 * The C++ promise interface is a little clumsy so this does some work in JS for us. This function
		 * is called once and returns a JS function that will be reused.
		 */
		static auto CompileAsyncWrapper() -> Local<Function> {
			Isolate* isolate = Isolate::GetCurrent();
			Local<Context> context = IsolateEnvironment::GetCurrent()->DefaultContext();
			Local<Script> script = Unmaybe(Script::Compile(context, v8_string(
				"'use strict';"
				"(function(AsyncCallback) {"
					"return function(ptr, did_finish, promise) {"
						"promise.then(function(val) {"
							"AsyncCallback(ptr, did_finish, val);"
						"}, function(err) {"
							"AsyncCallback(ptr, did_finish, null, err);"
						"});"
					"};"
				"})"
			)));
			Local<Value> outer_fn = Unmaybe(script->Run(context));
			assert(outer_fn->IsFunction());
			Local<Value> callback_fn = Unmaybe(FunctionTemplate::New(isolate, AsyncCallback)->GetFunction(context));
			Local<Value> inner_fn = Unmaybe(outer_fn.As<Function>()->Call(context, Undefined(isolate), 1, &callback_fn));
			assert(inner_fn->IsFunction());
			return inner_fn.As<Function>();
		}

		auto TransferArguments() -> std::vector<Local<Value>> {
			std::vector<Local<Value>> argv_inner;
			size_t argc = argv.size();
			argv_inner.reserve(argc);
			for (size_t ii = 0; ii < argc; ++ii) {
				argv_inner.emplace_back(argv[ii]->TransferIn());
			}
			return argv_inner;
		}

		std::vector<unique_ptr<Transferable>> argv;
		RemoteHandle<Context> context;
		RemoteHandle<Value> reference;
		unique_ptr<Transferable> recv;
		unique_ptr<Transferable> ret;
		uint32_t timeout = 0;
		// Only used in the AsyncPhase2 case
		shared_ptr<bool> did_finish;
		Transferable::Options return_transfer_options;
		unique_ptr<ExternalCopy> async_error;
		Scheduler::AsyncWait* async_wait = nullptr;
};
template <int async>
auto ReferenceHandle::Apply(MaybeLocal<Value> recv_handle, MaybeLocal<Array> maybe_arguments, MaybeLocal<Object> maybe_options) -> Local<Value> {
	return ThreePhaseTask::Run<async, ApplyRunner>(*isolate, *this, recv_handle, maybe_arguments, maybe_options);
}

/**
 * Copy this reference's value into this isolate
 */
class CopyRunner : public ThreePhaseTask {
	public:
		CopyRunner(
			const ReferenceHandle& that,
			RemoteHandle<Context> context,
			RemoteHandle<Value> reference
		) : context{std::move(context)}, reference{std::move(reference)} {
			that.CheckDisposed();
		}

		void Phase2() final {
			Context::Scope context_scope{Deref(context)};
			Local<Value> value = Deref(reference);
			copy = ExternalCopy::Copy(value);
		}

		auto Phase3() -> Local<Value> final {
			return copy->TransferIn();
		}

	private:
		RemoteHandle<Context> context;
		RemoteHandle<Value> reference;
		unique_ptr<Transferable> copy;
};

template <int async>
auto ReferenceHandle::Copy() -> Local<Value> {
	return ThreePhaseTask::Run<async, CopyRunner>(*isolate, *this, context, reference);
}

/**
 * Get a property from this reference, returned as another reference
 */
class GetRunner : public ThreePhaseTask {
	public:
		GetRunner(
			const ReferenceHandle& that,
			Local<Value> key_handle,
			MaybeLocal<Object> maybe_options
		) :
				context{that.context},
				reference{that.reference},
				options{maybe_options, Transferable::Options::Type::Reference} {
			that.CheckDisposed();
			key = ExternalCopy::CopyIfPrimitive(key_handle);
			if (!key) {
				throw js_type_error("Invalid `key`");
			}
		}

		void Phase2() final {
			Local<Context> context_handle = Deref(context);
			Context::Scope context_scope{context_handle};
			Local<Value> key_inner = key->CopyInto();
			Local<Object> object = Local<Object>::Cast(Deref(reference));
			Local<Value> value = Unmaybe(object->Get(context_handle, key_inner));
			ret = Transferable::TransferOut(value, options);
		}

		auto Phase3() -> Local<Value> final {
			return ret->TransferIn();
		}

	private:
		unique_ptr<ExternalCopy> key;
		RemoteHandle<Context> context;
		RemoteHandle<Value> reference;
		unique_ptr<Transferable> ret;
		Transferable::Options options;
};
template <int async>
auto ReferenceHandle::Get(Local<Value> key_handle, MaybeLocal<Object> maybe_options) -> Local<Value> {
	return ThreePhaseTask::Run<async, GetRunner>(*isolate, *this, key_handle, maybe_options);
}

/**
 * Attempt to set a property on this reference
 */
class SetRunner : public ThreePhaseTask {
	public:
		SetRunner(
			ReferenceHandle& that,
			Local<Value> key_handle,
			Local<Value> val_handle,
			MaybeLocal<Object> maybe_options
		) :
				key{ExternalCopy::CopyIfPrimitive(key_handle)},
				val{Transferable::TransferOut(val_handle, Transferable::Options{maybe_options})},
				context{that.context},
				reference{that.reference} {
			that.CheckDisposed();
			if (!key) {
				throw js_type_error("Invalid `key`");
			}
		}

		void Phase2() final {
			Local<Context> context_handle = Deref(context);
			Context::Scope context_scope{context_handle};
			Local<Value> key_inner = key->CopyInto();
			Local<Object> object = Local<Object>::Cast(Deref(reference));
			// Delete key before transferring in, potentially freeing up some v8 heap
			Unmaybe(object->Delete(context_handle, key_inner));
			Local<Value> val_inner = val->TransferIn();
			did_set = Unmaybe(object->Set(context_handle, key_inner, val_inner));
		}

		auto Phase3() -> Local<Value> final {
			return Boolean::New(Isolate::GetCurrent(), did_set);
		}

	private:
		unique_ptr<ExternalCopy> key;
		unique_ptr<Transferable> val;
		RemoteHandle<Context> context;
		RemoteHandle<Value> reference;
		bool did_set = false;
};
template <int async>
auto ReferenceHandle::Set(Local<Value> key_handle, Local<Value> val_handle, MaybeLocal<Object> maybe_options) -> Local<Value> {
	return ThreePhaseTask::Run<async, SetRunner>(*isolate, *this, key_handle, val_handle, maybe_options);
}

void ReferenceHandle::CheckDisposed() const {
	if (!reference) {
		throw js_generic_error("Reference has been released");
	}
}

/**
 * ReferenceHandleTransferable implementation
 */
auto ReferenceHandleTransferable::TransferIn() -> Local<Value> {
	return ClassHandle::NewInstance<ReferenceHandle>(std::move(*this));
}

} // namespace ivm
