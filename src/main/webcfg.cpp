/*
 * webcfg.cpp
 *
 *  Created on: 21. 7. 2019
 *      Author: ondra
 */


#include "webcfg.h"

#include <imtjson/array.h>
#include <imtjson/object.h>
#include <imtjson/string.h>
#include "../imtjson/src/imtjson/binary.h"
#include "../imtjson/src/imtjson/ivalue.h"
#include "../server/src/simpleServer/query_parser.h"
#include "../server/src/simpleServer/urlencode.h"
#include "../shared/ini_config.h"

using namespace json;
using ondra_shared::StrViewA;
using ondra_shared::IniConfig;
using namespace simpleServer;

NamedEnum<WebCfg::Command> WebCfg::strCommand({
	{WebCfg::all_pairs, "all_pairs"},
	{WebCfg::config, "config"},
	{WebCfg::restart, "restart"},
	{WebCfg::serialnr, "serial"},
	{WebCfg::info, "info"},
	{WebCfg::brokers, "brokers"}
});

WebCfg::WebCfg(const ondra_shared::IniConfig::Section &cfg,
		const std::string &realm,
		IStockSelector &stockSelector,
		std::function<void()> &&restart_fn)
	:auth(cfg["http_auth"].getString(), realm)
	,config_path(cfg["enabled"].getCurPath())
	,stockSelector(stockSelector)
	,restart_fn(std::move(restart_fn))
	,serial(cfg["serial"].getUInt())
{
	config_path.append(IniConfig::pathSeparator.data).append("webcfg.conf");
}

bool WebCfg::operator ()(const simpleServer::HTTPRequest &req,
		const ondra_shared::StrViewA &vpath) const {

	QueryParser qp(vpath);
	StrViewA path = qp.getPath();
	auto splt = path.split("/",3);
	splt();
	StrViewA pfx = splt();
	if (pfx != "api") return false;
	StrViewA c = splt();
	StrViewA rest = splt();
	auto cmd = strCommand.find(c);
	if (cmd == nullptr) {
		return false;
	} else {
		if (!auth.checkAuth(req)) return true;
		switch (*cmd) {
		case all_pairs: return reqAllPairs(req);
		case config: return reqConfig(req);
		case restart: return reqRestart(req);
		case serialnr: return reqSerial(req);
		case brokers: return reqBrokers(req, rest);
		case info: return reqInfo(req,qp["broker"],qp["symbol"]);
		}
	}
	return false;
}

bool WebCfg::reqAllPairs(simpleServer::HTTPRequest req) const {
	if (!req.allowMethods({"GET"})) return true;
	Object stocks;
	stockSelector.forEachStock([&](const std::string_view &name,IStockApi &stock){
		Array markets;
		auto m = stock.getAllPairs();
		for (auto &&k:m) markets.push_back(k);
		stocks.set(name, markets);
	});
	req.sendResponse("application/json",Value(stocks).stringify());
	return true;
}

bool WebCfg::reqConfig(simpleServer::HTTPRequest req) const {
	if (!req.allowMethods({"GET","PUT"})) return true;
	if (req.getMethod() == "GET") {

		IniConfig cfg;
		cfg.load(config_path);
		Object sections;
		for (auto &&k: cfg) {
			Object values;
			for (auto &&v: k.second) {
				values.set(v.first, v.second.getString());
			}
			sections.set(k.first, values);
		}
		req.sendResponse("application/json",Value(sections).stringify());

	} else {

		req.readBodyAsync(1024*1024, [config_path=this->config_path](simpleServer::HTTPRequest req) {
			Value data = Value::fromString(StrViewA(BinaryView(req.getUserBuffer())));
			std::ofstream out(config_path, std::ios::out| std::ios::trunc);
			for (Value sect : data) {
				out << "[" << sect.getKey() << "]" << std::endl;;
				for (Value z : sect) {
					out << z.getKey() << "=" << z.toString() << std::endl;
				}
				out << std::endl;
			}
			if (!out) {
				req.sendErrorPage(500);
			} else {
				req.sendErrorPage(202);
			}
		});


	}
	return true;


}

bool WebCfg::reqRestart(simpleServer::HTTPRequest req) const {
	if (!req.allowMethods({"POST"})) return true;
	restart_fn();
	req.sendErrorPage(201);
	return true;
}

bool WebCfg::reqSerial(simpleServer::HTTPRequest req) const {
	if (!req.allowMethods({"GET"})) return true;
	req.sendResponse("text/plain") << serial << "\r\n";
	return true;
}


static double getSafeBalance(IStockApi *api, std::string_view symb) {
	try {
		return api->getBalance(symb);
	} catch (...) {
		return 0;
	}
}

bool WebCfg::reqInfo(simpleServer::HTTPRequest req, ondra_shared::StrViewA broker, ondra_shared::StrViewA symbol) const {
	if (!req.allowMethods({"GET"})) return true;
	IStockApi *api = stockSelector.getStock(broker);
	if (api == nullptr) return false;
	try {
		IStockApi::MarketInfo nfo = api->getMarketInfo(symbol);
		double ab = getSafeBalance(api,nfo.asset_symbol);
		double cb = getSafeBalance(api,nfo.currency_symbol);
		Value resp = Object
					("asset_symbol",nfo.asset_symbol)
					("currency_symbol", nfo.currency_symbol)
					("fees", nfo.fees)
					("leverage", nfo.leverage)
					("asset_balance", ab)
					("currency_balance", cb)
					("price", api->getTicker(symbol).last);
		req.sendResponse("application/json",resp.stringify());
	} catch (...) {
		return false;
	}
	return true;
}

bool WebCfg::reqBrokers(simpleServer::HTTPRequest req, ondra_shared::StrViewA rest) const {
	if (!req.allowMethods({"GET"})) return true;
	if (rest.empty()) {
		Array brokers;
		stockSelector.forEachStock([&](const std::string_view &name,IStockApi &){
			brokers.push_back(name);
		});
		Object obj("entries", brokers);
		req.sendResponse("application/json",Value(obj).stringify());
	} else {
		auto splt = rest.split("/",3);
		StrViewA urlbroker = splt();
		StrViewA entry = splt();
		rest = splt();
		std::string broker = urlDecode(urlbroker);
		IStockApi *api = stockSelector.getStock(broker);
		if (api == nullptr) {
			req.sendErrorPage(404);
			return true;
		}

		if (entry.empty()) {
			auto binfo  = api->getBrokerInfo();
			Value res = Object("name", binfo.name)
							("exchangeName", binfo.exchangeName)
							("exchangeUrl", binfo.exchangeUrl)
							("version", binfo.version)
							("licence", binfo.licence)
							("entries", {"icon.png","pairs"});
			req.sendResponse("application/json",res.stringify());
		} else if (entry == "icon.png") {
			auto binfo  = api->getBrokerInfo();
			Value v = base64->decodeBinaryValue(binfo.favicon);
			Binary b = v.getBinary(base64);
			req.sendResponse(HTTPResponse(200).contentType("image/png").cacheFor(600),
					StrViewA(b));
		} else if (entry == "pairs"){
			if (rest.empty()) {
				Array p;
				auto pairs = api->getAllPairs();
				for (auto &&k: pairs) p.push_back(k);
				Object obj("entries", p);
				req.sendResponse("application/json",Value(obj).stringify());
			} else {
				std::string pair = urlDecode(rest);
				IStockApi::MarketInfo nfo = api->getMarketInfo(pair);
				double ab = getSafeBalance(api,nfo.asset_symbol);
				double cb = getSafeBalance(api,nfo.currency_symbol);
				Value resp = Object
							("asset_symbol",nfo.asset_symbol)
							("currency_symbol", nfo.currency_symbol)
							("fees", nfo.fees)
							("leverage", nfo.leverage)
							("asset_balance", ab)
							("currency_balance", cb)
							("price", api->getTicker(pair).last);
				req.sendResponse("application/json",resp.stringify());
			}
		}
	}
	return true;

}