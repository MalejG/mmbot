/*
 * strategy_keepvalue.cpp
 *
 *  Created on: 20. 10. 2019
 *      Author: ondra
 */

#include "strategy_leveraged_base.h"

#include <chrono>
#include <imtjson/object.h>
#include "../shared/logOutput.h"
#include <cmath>

#include "../imtjson/src/imtjson/string.h"
#include "sgn.h"


using ondra_shared::logDebug;

template<typename Calc>
std::string_view Strategy_Leveraged<Calc>::id = Calc::id;

template<typename Calc>
Strategy_Leveraged<Calc>::Strategy_Leveraged(const PCalc &calc, const PConfig &cfg, State &&st)
:calc(calc),cfg(cfg), st(std::move(st)) {}
template<typename Calc>
Strategy_Leveraged<Calc>::Strategy_Leveraged(const PCalc &calc, const PConfig &cfg)
:calc(calc),cfg(cfg) {}


template<typename Calc>
bool Strategy_Leveraged<Calc>::isValid() const {
	return st.neutral_price > 0 && st.power > 0 && st.last_price > 0 && st.bal+cfg->external_balance > 0 && cfg->external_balance+st.redbal > 0
			&& std::isfinite(st.val) && std::isfinite(st.neutral_price) && std::isfinite(st.power) && std::isfinite(st.bal) && std::isfinite(st.redbal) && std::isfinite(st.last_price) && std::isfinite(st.neutral_pos);
}

template<typename Calc>
void Strategy_Leveraged<Calc>::recalcNewState(const PCalc &calc, const PConfig &cfg, State &nwst) {
	double adjbalance = std::abs(nwst.bal + cfg->external_balance) * cfg->power;
	nwst.power = calc->calcPower(nwst.last_price, adjbalance, cfg->asym);
	recalcNeutral(calc,cfg,nwst);
	for (int i = 0; i < 100; i++) {
		nwst.power = calc->calcPower(nwst.neutral_price, adjbalance, cfg->asym);
		recalcNeutral(calc,cfg,nwst);
	}
	nwst.val = calc->calcPosValue(nwst.power, calcAsym(cfg,nwst), nwst.neutral_price, nwst.last_price);
	nwst.redbal = nwst.bal;
}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::init(const PCalc &calc, const PConfig &cfg, double price, double pos, double currency, const IStockApi::MarketInfo &minfo) {
	bool futures = minfo.leverage != 0 || cfg->longonly;
	auto bal = getBalance(*cfg,futures, price, pos, currency);
	State nwst {
		/*neutral_price:*/ price,
		/*last_price */ price,
		/*position */ pos - bal.second,
		/*bal */ bal.first-cfg->external_balance,
		/* val */ 0,
		/*redbal*/ bal.first-cfg->external_balance,
		/* power */ 0,
		/* neutral_pos */bal.second
	};
	if (nwst.bal+cfg->external_balance<= 0) {
		//we cannot calc with empty balance. In this case, use price for calculation (but this is  unreal, trading still impossible)
		nwst.bal = price;
	}
	PCalc newcalc = calc;
	if (!newcalc->isValid(minfo)) newcalc = std::make_shared<Calc>(calc->init(minfo));
	recalcNewState(newcalc, cfg,nwst);

	auto res = PStrategy(new Strategy_Leveraged (newcalc, cfg, std::move(nwst)));
	if (!res->isValid())  {
		throw std::runtime_error("Unable to initialize strategy - invalid configuration");
	}
	return res;
}




template<typename Calc>
double Strategy_Leveraged<Calc>::calcPosition(double price) const {
	if (cfg->max_loss) {
		auto mm = calcRoots();
		if (price < mm.min) {
			price = mm.min;
		}
		if (price > mm.max) {
			price = mm.max;
		}
	}

	double reduction = cfg->reduction;
	double mprice = calc->calcPrice0(st.neutral_price, calcAsym());
	double dynred = 0;
	if (cfg->dynred) {
		auto mm = calcRoots();
		double distance = pow2((price - mprice)/(mm.max - mm.min));
		dynred = pow2(distance*cfg->dynred);
	}
	if (dynred > 1.0) dynred = 1.0;
	reduction = std::sqrt(pow2(reduction) + 0.5*dynred);
	double new_neutral;

	double profit = st.position * (price - st.last_price);
	{
		//NOTE: always reduce when price is going up
		//because we need to reduce risk from short (so reduce when opening short position)
		//and we reduce opened long position as well
		//
		//for inverted futures, short and long is swapped
		if (reduction && price > st.last_price) {
			profit += st.bal - st.redbal;
			new_neutral = calcNewNeutralFromProfit(profit, price,reduction);
		} else {
			new_neutral = st.neutral_price;
		}
	}
	double pos = calc->calcPosition(st.power, calcAsym(), new_neutral, price);
	double initpos = calc->calcPosition(st.power,calcAsym(), new_neutral, new_neutral);
	if ((initpos - st.position) * (initpos - pos) <= 0) {
			pos = (pos - initpos) * std::pow(2.0,cfg->initboost) + initpos;
	}
	if (cfg->longonly && pos < 0) pos = 0;

	return pos;

}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::onIdle(
		const IStockApi::MarketInfo &minfo,
		const IStockApi::Ticker &ticker, double assets, double currency) const {
	if (isValid()) {
		if (st.power <= 0) {
			State nst = st;
			recalcNewState(calc, cfg, nst);
			return new Strategy_Leveraged<Calc>(calc, cfg, std::move(nst));
		} else {
			return this;
		}
	}
	else {
		return init(calc, cfg,ticker.last, assets, currency, minfo);
	}
}

template<typename Calc>
double Strategy_Leveraged<Calc>::calcNewNeutralFromProfit(double profit, double price, double reduction) const {

	double asym = calcAsym();
	double middle = calc->calcPrice0(st.neutral_price, asym);
	if ((middle - st.last_price ) * (middle - price) <= 0)
		return st.neutral_price;


	double new_val;
	bool rev_shift = ((price >= middle && price <= st.neutral_price) || (price <= middle && price >= st.neutral_price));
	double prev_val = st.val;
	new_val = prev_val - profit;
	double c_neutral;
	double neutral_from_price = calc->calcNeutralFromPrice0(price, asym);
	if (calc->calcPosValue(st.power, asym, neutral_from_price, price) > new_val) {
		c_neutral = neutral_from_price;
	} else {
		c_neutral = calc->calcNeutralFromValue(st.power, asym, st.neutral_price, new_val, price);
		if (rev_shift) {
			c_neutral = 2*st.neutral_price - c_neutral;
		}
	}
	double new_neutral = st.neutral_price + (c_neutral - st.neutral_price)* 2 * (reduction);
	return new_neutral;
}

template<typename Calc>
void Strategy_Leveraged<Calc>::recalcPower(const PCalc &calc, const PConfig &cfg, State &nwst) {
	double offset = calc->calcPosition(nwst.power, cfg->asym, nwst.neutral_price, nwst.neutral_price);
	double adjbalance = std::abs(nwst.redbal  + cfg->external_balance + nwst.neutral_price * std::abs(nwst.position - offset) * cfg->powadj) * cfg->power;
	double power = calc->calcPower(nwst.neutral_price, adjbalance, cfg->asym);
	if (std::isfinite(power)) {
		nwst.power = power;
	}
}

template<typename Calc>
void Strategy_Leveraged<Calc>::recalcNeutral(const PCalc &calc, const PConfig &cfg,State &nwst)  {
	double neutral_price = calc->calcNeutral(nwst.power, calcAsym(cfg,nwst), nwst.position,
			nwst.last_price);
	if (std::isfinite(neutral_price) && neutral_price > 0) {
		nwst.neutral_price = neutral_price;
	}
}

template<typename Calc>
std::pair<typename Strategy_Leveraged<Calc>::OnTradeResult, PStrategy> Strategy_Leveraged<Calc>::onTrade(
		const IStockApi::MarketInfo &minfo,
		double tradePrice, double tradeSize, double assetsLeft,
		double currencyLeft) const {


	if (!isValid()) {
		return init(calc, cfg,tradePrice, assetsLeft, currencyLeft, minfo)
				->onTrade(minfo, tradePrice, tradeSize, assetsLeft, currencyLeft);
	}

	State nwst = st;
	if (tradeSize * st.position < 0 && (st.position + tradeSize)/st.position > 0.5) {
		int chg = sgn(tradeSize);
		nwst.trend_cntr += chg - nwst.trend_cntr/1000;

	}
	double apos = assetsLeft - st.neutral_pos;
	auto cpos = calcPosition(tradePrice);
	double mult = st.power;
	double profit = (apos - tradeSize) * (tradePrice - st.last_price);
	double vprofit = (st.position) * (tradePrice - st.last_price);
	//store current position
	nwst.position = cpos;
	//store last price
	nwst.last_price = tradePrice;

	recalcNeutral(calc, cfg, nwst);

	double val = calc->calcPosValue(mult, calcAsym(), nwst.neutral_price, tradePrice);
	//calculate extra profit - we get change of value and add profit. This shows how effective is strategy. If extra is positive, it generates
	//profit, if it is negative, is losses
	double extra = (val - st.val) + profit;
	//store val to calculate next profit (because strategy was adjusted)
	nwst.val = val;


	//store new balance
	nwst.bal += (val - st.val) + vprofit;

	if  (nwst.last_price > st.last_price) {
		if (cfg->reinvest_profit) {
			nwst.redbal = nwst.bal;
		} else {
			nwst.bal = nwst.redbal;
		}
	}


	recalcPower(calc, cfg, nwst);

	return {
		OnTradeResult{extra,0,calc->calcPrice0(st.neutral_price, calcAsym()),0},
		new Strategy_Leveraged<Calc>(calc, cfg,  std::move(nwst))
	};

}

template<typename Calc>
json::Value Strategy_Leveraged<Calc>::storeCfgCmp() const {
	return json::Object("asym", static_cast<int>(cfg->asym * 1000))("ebal",
			static_cast<int>(cfg->external_balance * 1000))("power",
			static_cast<int>(cfg->power * 1000))("lo",cfg->longonly);
}

template<typename Calc>
json::Value Strategy_Leveraged<Calc>::exportState() const {
	return json::Object
			("neutral_price",st.neutral_price)
			("last_price",st.last_price)
			("position",st.position)
			("balance",st.bal)
			("val",st.val)
			("redbal",st.redbal)
			("power",st.power)
			("neutral_pos",st.neutral_pos)
			("trend", st.trend_cntr)
			("cfg", storeCfgCmp());

}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::importState(json::Value src,const IStockApi::MarketInfo &minfo) const {
		State newst {
			src["neutral_price"].getNumber(),
			src["last_price"].getNumber(),
			src["position"].getNumber(),
			src["balance"].getNumber(),
			src["val"].getNumber(),
			src["redbal"].getNumber(),
			src["power"].getNumber(),
			src["neutral_pos"].getNumber(),
			src["trend"].getInt()
		};
		json::Value cfgcmp = src["cfg"];
		json::Value cfgcmp2 = storeCfgCmp();
		if (cfgcmp != cfgcmp2) {
			double last_price = newst.last_price;
			if (cfg->recalc_keep_neutral) {
				newst.last_price = calc->calcPrice0(newst.neutral_price, calcAsym(cfg, newst));
				newst.position = 0;
				recalcNewState(calc, cfg,newst);
				newst.last_price = last_price;
				newst.position = calc->calcPosition(newst.power,calcAsym(cfg,newst),newst.neutral_price, last_price);
			} else {
				recalcNewState(calc, cfg,newst);
			}
			newst.val= calc->calcPosValue(newst.power,calcAsym(cfg,newst),newst.neutral_price, last_price);
		}
		PCalc newcalc = calc;
		if (!newcalc->isValid(minfo)) newcalc = std::make_shared<Calc>(newcalc->init(minfo));
		return new Strategy_Leveraged<Calc>(newcalc, cfg, std::move(newst));
}

template<typename Calc>
IStrategy::OrderData Strategy_Leveraged<Calc>::getNewOrder(
		const IStockApi::MarketInfo &minfo,
		double curPrice, double price, double dir, double assets, double currency, bool rej) const {
	auto apos = assets - st.neutral_pos;
	double asym = calcAsym(cfg,st);
	if (cfg->max_loss && (curPrice < calcRoots().min || curPrice > calcRoots().max)) {
		auto testStat = onTrade(minfo,curPrice,0,assets,currency);
		auto mm2 = static_cast<const Strategy_Leveraged<Calc> *>((const IStrategy *)(testStat.second))->calcRoots();
		if (dir * apos < 0 && (curPrice < mm2.min || curPrice > mm2.max))
			return {curPrice,-apos,Alert::stoploss};
		else
			return {0,0,Alert::stoploss};
	} else {
		double bal = (st.bal+cfg->external_balance);;
		double lev = std::abs(st.position) * st.last_price / bal;
		if (!rej && ((lev > 0.5 && st.redbal != st.bal) || lev>2)  && st.val > 0) {
			if (cfg->fastclose && dir * st.position < 0) {
				double midl = calc->calcPrice0(st.neutral_price, asym);
				double calc_price = (price - midl) * (st.last_price - midl) < 0?midl:price;
				double newval = calc->calcPosValue(st.power, asym,st.neutral_price, calc_price);
				double valdiff = st.val - newval;
				if (valdiff > 0) {
					double fastclose_delta = valdiff/st.position;
					double close_price = fastclose_delta+st.last_price;
					if (close_price * dir < curPrice * dir && close_price * dir > price * dir) {
						price = close_price;

						auto cps = calcPosition(close_price);
						double newlev = std::abs(cps)*close_price / bal;
						if (lev > 2 && st.bal != st.redbal) {
							int cnt = 20;
							 while (newlev < lev - 1 && cnt) {
								 close_price = (close_price + st.last_price)*0.5;
								auto cps = calcPosition(close_price);
								 newlev = std::abs(cps)*close_price / bal;
								 cnt --;
							 }
							 if (cnt) {
								 price = close_price;
							 }

						}

					}
				}
			}
			if (cfg->slowopen && dir * st.position > 0) {
				double newval = calc->calcPosValue(st.power, asym,st.neutral_price, price);
				double valdiff = newval - st.val;
				if (valdiff > 0) {
					double delta = -valdiff/st.position;
					double open_price = delta + st.last_price;
					if (open_price * dir < price * dir &&  price > 0) {
						logDebug("Slow open active: valdiff=$1, delta=$2, spread=$3",
								valdiff, delta, price-st.last_price);
						price = open_price;
					}

				}
			}

		}


		auto cps = calcPosition(price);
		double df = calcOrderSize(st.position,apos,cps);
		return {price, df,  cps == 0?Alert::forced:Alert::enabled};
	}
}

template<typename Calc>
typename Strategy_Leveraged<Calc>::MinMax Strategy_Leveraged<Calc>::calcSafeRange(
		const IStockApi::MarketInfo &minfo,
		double assets,
		double currencies) const {

	if (minfo.leverage) {
		return calcRoots();
	} else {
		auto r = calcRoots();
		if (cfg->longonly) r.max = calc->calcPrice0(st.neutral_price, calcAsym());
		double maxp = calc->calcPriceFromPosition(st.power, calcAsym(), st.neutral_price, -st.neutral_pos);
		double minp = calc->calcRoots(st.power, calcAsym(),st.neutral_price, currencies).min;
		return {std::max(r.min,minp),std::min(r.max,maxp)};
	}
}

template<typename Calc>
double Strategy_Leveraged<Calc>::getEquilibrium(double assets) const {
	return  calc->calcPriceFromPosition(st.power, calcAsym(), st.neutral_price, assets-st.neutral_pos);
}

template<typename Calc>
PStrategy Strategy_Leveraged<Calc>::reset() const {
	return new Strategy_Leveraged<Calc>(calc, cfg,{});
}

template<typename Calc>
json::Value Strategy_Leveraged<Calc>::dumpStatePretty(
		const IStockApi::MarketInfo &minfo) const {

	return json::Object("Position", (minfo.invert_price?-1:1)*st.position)
				  ("Last price", minfo.invert_price?1/st.last_price:st.last_price)
				 ("Neutral price", minfo.invert_price?1/st.neutral_price:st.neutral_price)
				 ("Value", st.val)
				 ("Normalized PnL", st.bal)
				 ("Normalized unused PnL", st.bal - st.redbal)
				 ("Multiplier", st.power)
				 ("Neutral pos", st.neutral_pos?json::Value(st.neutral_pos):json::Value())
	 	 	 	 ("Trend factor", json::String({
						json::Value((minfo.invert_price?-1:1)*trendFactor(st)*100).toString(),"%"}));


}



template<typename Calc>
double Strategy_Leveraged<Calc>::calcMaxLoss() const {
	double lmt;
	if (cfg->max_loss == 0)
		lmt = cfg->external_balance+st.bal;
	else
		lmt = cfg->max_loss;

	if (st.val < 0)
		lmt += st.val;
	return lmt;
}

template<typename Calc>
typename Strategy_Leveraged<Calc>::MinMax Strategy_Leveraged<Calc>::calcRoots() const {
	if (!rootsCache.has_value()) {
		double lmt = calcMaxLoss();
		rootsCache = calc->calcRoots(st.power, calcAsym(),st.neutral_price,lmt);
	}
	return *rootsCache;
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcAsym(const PConfig &cfg, const State &st)  {
	if (cfg->detect_trend) {
		return cfg->asym * trendFactor(st);
	}
	else {
		return cfg->asym;
	}
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcAsym() const {
	return calcAsym(cfg,st);
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::trendFactor(const State &st) {
	return st.trend_cntr*0.001;
}

template<typename Calc>
std::pair<double,double> Strategy_Leveraged<Calc>::getBalance(const Config &cfg, bool leveraged, double price, double assets, double currency) {
	if (leveraged) {
		if (cfg.external_balance) return {cfg.external_balance, 0};
		else return {currency, 0};
	} else {
		double md = assets + currency / price;
		double bal = cfg.external_balance?cfg.external_balance:(md * price)/2;
		return {bal, md/2};
	}
}

template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcInitialPosition(const IStockApi::MarketInfo &minfo, double price, double assets, double currency) const {
	if (!isValid()) {
		return init(calc, cfg, price, assets, currency, minfo)->calcInitialPosition(minfo,price,assets,currency);
	} else {
		return calcPosition(st.neutral_price);
	}
}

template<typename Calc>
typename Strategy_Leveraged<Calc>::BudgetInfo Strategy_Leveraged<Calc>::getBudgetInfo() const {
	return {st.bal + cfg->external_balance, 0};
}


template<typename Calc>
inline double Strategy_Leveraged<Calc>::calcCurrencyAllocation(double) const {
	return cfg->external_balance + st.redbal - st.val;
}


