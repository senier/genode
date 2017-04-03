/*
 * \brief  Server role of init, forwarding session requests to children
 * \author Norman Feske
 * \date   2017-03-07
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <os/session_policy.h>

/* local includes */
#include <server.h>


/***************************
 ** Init::Server::Service **
 ***************************/

struct Init::Server::Service
{
	Registry<Service>::Element _registry_element;

	Buffered_xml _service_node;

	typedef Genode::Service::Name Name;

	Registry<Routed_service> &_child_services;

	Name const _name { _service_node.xml().attribute_value("name", Name()) };

	/**
	 * Constructor
	 *
	 * \param alloc  allocator used for buffering the 'service_node'
	 */
	Service(Registry<Service>        &services,
	        Allocator                &alloc,
	        Xml_node                  service_node,
	        Registry<Routed_service> &child_services)
	:
		_registry_element(services, *this),
		_service_node(alloc, service_node),
		_child_services(child_services)
	{ }

	/**
	 * Determine route to child service for a given label according
	 * to the <service> node policy
	 *
	 * \throw Parent::Service_denied
	 */
	Route resolve_session_request(Session_label const &);

	Name name() const { return _name; }
};


Init::Server::Route
Init::Server::Service::resolve_session_request(Session_label const &label)
{
	try {
		Session_policy policy(label, _service_node.xml());

		if (!policy.has_sub_node("child"))
			throw Parent::Service_denied();

		Xml_node target_node = policy.sub_node("child");

		Child_policy::Name const child_name =
			target_node.attribute_value("name", Child_policy::Name());

		typedef String<Session_label::capacity()> Label;
		Label const target_label =
			target_node.attribute_value("label", Label(label.string()));

		Routed_service *match = nullptr;
		_child_services.for_each([&] (Routed_service &service) {
			if (service.child_name() == child_name && service.name() == name())
				match = &service; });

		if (!match || match->abandoned())
			throw Parent::Service_denied();

		return Route { *match, target_label };
	}
	catch (Session_policy::No_policy_defined) {
		throw Parent::Service_denied(); }
}


/******************
 ** Init::Server **
 ******************/

Init::Server::Route
Init::Server::_resolve_session_request(Service::Name const &service_name,
                                       Session_label const &label)
{
	Service *matching_service = nullptr;
	_services.for_each([&] (Service &service) {
		if (service.name() == service_name)
			matching_service = &service; });

	if (!matching_service)
		throw Parent::Service_denied();

	return matching_service->resolve_session_request(label);
}


static void close_session(Genode::Session_state &session)
{
	session.phase = Genode::Session_state::CLOSE_REQUESTED;
	session.service().initiate_request(session);
	session.service().wakeup();
}


void Init::Server::session_ready(Session_state &session)
{
	_report_update_trigger.trigger_report_update();

	/*
	 * If 'session_ready' is called as response to a session-quota upgrade,
	 * the 'phase' is set to 'CAP_HANDED_OUT' by 'Child::session_response'.
	 * We just need to forward the state change to our parent.
	 */
	if (session.phase == Session_state::CAP_HANDED_OUT) {
		Parent::Server::Id id { session.id_at_client().value };
		_env.parent().session_response(id, Parent::SESSION_OK);
	}

	if (session.phase == Session_state::AVAILABLE) {
		Parent::Server::Id id { session.id_at_client().value };
		_env.parent().deliver_session_cap(id, session.cap);
		session.phase = Session_state::CAP_HANDED_OUT;
	}
}


void Init::Server::session_closed(Session_state &session)
{
	_report_update_trigger.trigger_report_update();

	Parent::Server::Id id { session.id_at_client().value };
	_env.parent().session_response(id, Parent::SESSION_CLOSED);

	Ram_session_client(session.service().ram())
		.transfer_quota(_env.ram_session_cap(), session.donated_ram_quota());

	session.destroy();
}


void Init::Server::_handle_create_session_request(Xml_node request,
                                                  Parent::Client::Id id)
{
	if (!request.has_sub_node("args"))
		return;

	typedef Session_state::Args Args;
	Args const args = request.sub_node("args").decoded_content<Args>();

	Service::Name const name = request.attribute_value("service", Service::Name());

	Session_label const label = label_from_args(args.string());

	try {
		Route const route = _resolve_session_request(name, label);

		/*
		 * Reduce session quota by local session costs
		 */
		char argbuf[Parent::Session_args::MAX_SIZE];
		strncpy(argbuf, args.string(), sizeof(argbuf));

		size_t const ram_quota  = Arg_string::find_arg(argbuf, "ram_quota").ulong_value(0);
		size_t const keep_quota = route.service.factory().session_costs();

		if (ram_quota < keep_quota)
			throw Genode::Service::Quota_exceeded();

		size_t const forward_ram_quota = ram_quota - keep_quota;

		Arg_string::set_arg(argbuf, sizeof(argbuf), "ram_quota", forward_ram_quota);

		Session_state &session =
			route.service.create_session(route.service.factory(),
		                                 _client_id_space, id,
		                                 route.label, argbuf, Affinity());

		/* transfer session quota */
		if (_env.ram().transfer_quota(route.service.ram(), ram_quota)) {

			/*
			 * This should never happen unless our parent missed to
			 * transfor the session quota to us prior issuing the session
			 * request.
			 */
			warning("unable to transfer session quota (", ram_quota, " bytes) "
			        "of forwarded ", name, " session");
			session.destroy();
			throw Parent::Service_denied();
		}

		session.ready_callback  = this;
		session.closed_callback = this;

		/* initiate request */
		route.service.initiate_request(session);

		/* if request was not handled synchronously, kick off async operation */
		if (session.phase == Session_state::CREATE_REQUESTED)
			route.service.wakeup();

		if (session.phase == Session_state::INVALID_ARGS)
			throw Parent::Service_denied();

		if (session.phase == Session_state::QUOTA_EXCEEDED)
			throw Genode::Service::Quota_exceeded();
	}
	catch (Parent::Service_denied) {
		_env.parent().session_response(Parent::Server::Id { id.value }, Parent::INVALID_ARGS); }
	catch (Genode::Service::Quota_exceeded) {
		_env.parent().session_response(Parent::Server::Id { id.value }, Parent::QUOTA_EXCEEDED); }
}


void Init::Server::_handle_upgrade_session_request(Xml_node request,
                                                   Parent::Client::Id id)
{
	_client_id_space.apply<Session_state>(id, [&] (Session_state &session) {

		size_t const ram_quota = request.attribute_value("ram_quota", 0UL);

		session.phase = Session_state::UPGRADE_REQUESTED;

		if (_env.ram().transfer_quota(session.service().ram(), ram_quota)) {
			warning("unable to upgrade session quota (", ram_quota, " bytes) "
			        "of forwarded ", session.service().name(), " session");
			return;
		}

		session.increase_donated_quota(ram_quota);
		session.service().initiate_request(session);
		session.service().wakeup();
	});
}


void Init::Server::_handle_close_session_request(Xml_node request,
                                                 Parent::Client::Id id)
{
	_client_id_space.apply<Session_state>(id, [&] (Session_state &session) {
		close_session(session); });
}


void Init::Server::_handle_session_request(Xml_node request)
{
	if (!request.has_attribute("id"))
		return;

	/*
	 * We use the 'Parent::Server::Id' of the incoming request as the
	 * 'Parent::Client::Id' of the forwarded request.
	 */
	Parent::Client::Id const id { request.attribute_value("id", 0UL) };

	if (request.has_type("create"))
		_handle_create_session_request(request, id);

	if (request.has_type("upgrade"))
		_handle_upgrade_session_request(request, id);

	if (request.has_type("close"))
		_handle_close_session_request(request, id);
}


void Init::Server::_handle_session_requests()
{
	_session_requests->update();

	Xml_node const requests = _session_requests->xml();

	requests.for_each_sub_node([&] (Xml_node request) {
	 _handle_session_request(request); });

	_report_update_trigger.trigger_report_update();
}


void Init::Server::apply_config(Xml_node config)
{
	_services.for_each([&] (Service &service) { destroy(_alloc, &service); });

	config.for_each_sub_node("service", [&] (Xml_node node) {
		new (_alloc) Service(_services, _alloc, node, _child_services); });

	/*
	 * Construct mechanics for responding to our parent's session requests
	 * on demand.
	 */
	bool services_provided = false;
	_services.for_each([&] (Service const &) { services_provided = true; });

	if (services_provided && !_session_requests.constructed()) {
		_session_requests.construct(_env, "session_requests");
		_session_request_handler.construct(_env.ep(), *this,
		                                   &Server::_handle_session_requests);
		_session_requests->sigh(*_session_request_handler);

		if (_session_requests.constructed())
			_handle_session_requests();
	}

	/*
	 * Re-validate routes of existing sessions, close sessions whose routes
	 * changed.
	 */
	_client_id_space.for_each<Session_state>([&] (Session_state &session) {
		try {
			Route const route = _resolve_session_request(session.service().name(),
			                                             session.client_label());

			bool const route_unchanged = (route.service == session.service())
			                          && (route.label == session.label());
			if (!route_unchanged)
				throw Parent::Service_denied();
		}
		catch (Parent::Service_denied) {
			close_session(session); }
	});
}